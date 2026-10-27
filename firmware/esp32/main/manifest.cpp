#include "storage.h"
#include "protocol.h"
#include <math.h>
#include "cJSON.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static void* json_allocate(size_t bytes) {
    return heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
}
#endif
using namespace galaxy;
void init_manifest_allocator() {
#ifdef ESP_PLATFORM
    cJSON_Hooks hooks={json_allocate,heap_caps_free};
    cJSON_InitHooks(&hooks);
#endif
}
static const cJSON* get(const cJSON* j,const char* key) {return cJSON_GetObjectItemCaseSensitive(j,key);}
static bool number(const cJSON* j,const char* key,uint32_t& v) {
    const cJSON* n=get(j,key);
    if(!cJSON_IsNumber(n)||!isfinite(n->valuedouble)||n->valuedouble<0||
        n->valuedouble>UINT32_MAX||floor(n->valuedouble)!=n->valuedouble) return false;
    v=uint32_t(n->valuedouble);return true;
}
static bool equal(const cJSON* j,const char* key,const char* expected) {
    const cJSON* v=get(j,key);return cJSON_IsString(v)&&!strcmp(v->valuestring,expected);
}
bool parse_dataset(const char* json,Dataset& dataset) {
    // Bound nesting before the recursive JSON parser runs on the MCU stack.
    if(!json||strlen(json)>128*1024) return false;
    unsigned depth=0;bool quoted=false,escaped=false;
    for(const char* p=json;*p;p++) {
        if(quoted) {
            if(escaped) escaped=false;
            else if(*p=='\\') escaped=true;
            else if(*p=='"') quoted=false;
        } else if(*p=='"') quoted=true;
        else if(*p=='{'||*p=='[') {if(++depth>16) return false;}
        else if(*p=='}'||*p==']') {if(!depth) return false;--depth;}
    }
    if(quoted||depth) return false;
    cJSON* root=cJSON_ParseWithOpts(json,nullptr,true);if(!root) return false;
    bool valid=false;
    do {
        uint32_t version=0,w=0,h=0;
        if(!equal(root,"format","indicator-galaxy")||!equal(root,"pixel_format","RGB565_BE")||
           !equal(root,"layout","column-major")||!number(root,"version",version)||version!=1||
           !number(root,"width",w)||w!=480||!number(root,"height",h)||h!=480) break;
        const cJSON* bands=get(root,"bands");int n=cJSON_GetArraySize(bands);
        if(!cJSON_IsArray(bands)||n<1||n>9999) break;
        bool ok=true;
        for(int i=0;i<n && ok;i++) {
            const cJSON* entry=cJSON_GetArrayItem(bands,i);Band b;
            if(!number(entry,"width",b.width)||b.width<480||b.width>UINT32_MAX-480||
               !number(entry,"valid_height",b.valid_height)||!b.valid_height||b.valid_height>480) {ok=false;break;}
            const cJSON* chunks=get(entry,"chunks");int nc=cJSON_GetArraySize(chunks);
            if(!cJSON_IsArray(chunks)||nc<1||nc>1000) {ok=false;break;}
            uint64_t first=0;
            for(int k=0;k<nc;k++) {
                const cJSON* c=cJSON_GetArrayItem(chunks,k);const cJSON* f=get(c,"file");Chunk chunk;
                if(!cJSON_IsString(f)||strlen(f->valuestring)>12) {ok=false;break;}
                chunk.file=f->valuestring;
                std::string path="/galaxy/"+dataset.name+"/"+chunk.file;
                if(!path_valid(path.c_str())||!number(c,"first_column",chunk.first)||chunk.first!=first||
                   !number(c,"columns",chunk.columns)||!chunk.columns||!number(c,"bytes",chunk.bytes)||
                   uint64_t(chunk.columns)*960!=chunk.bytes) {ok=false;break;}
                first+=chunk.columns;if(first>b.width) {ok=false;break;}
                b.chunks.push_back(std::move(chunk));
            }
            if(first!=b.width) ok=false;
            if(ok) dataset.bands.push_back(std::move(b));
        }
        if(!ok) break;
        const cJSON* title=get(root,"title"),*credit=get(root,"credit");
        dataset.title=cJSON_IsString(title)?title->valuestring:dataset.name;
        dataset.credit=cJSON_IsString(credit)?credit->valuestring:"";
        valid=true;
    } while(false);
    cJSON_Delete(root);return valid;
}
