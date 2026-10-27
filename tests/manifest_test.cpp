#include "../firmware/esp32/main/storage.h"
#include <assert.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdio.h>
static std::string valid=R"({"format":"indicator-galaxy","version":1,"width":480,"height":480,
 "pixel_format":"RGB565_BE","layout":"column-major","title":"Fixture",
 "bands":[{"width":500,"valid_height":480,"chunks":[
 {"file":"b0000000.dat","first_column":0,"columns":200,"bytes":192000},
 {"file":"b0000001.dat","first_column":200,"columns":300,"bytes":288000}]}]})";
static bool accepted(std::string json) {Dataset d;d.name="fixture";return parse_dataset(json.c_str(),d);}
static std::string changed(const char* from,const char* to) {
    std::string s=valid;size_t p=s.find(from);assert(p!=std::string::npos);s.replace(p,strlen(from),to);return s;
}
int main(int argc,char** argv) {
    assert(accepted(valid));
    assert(!accepted("garbage"));
    assert(!accepted(valid+" trailing junk"));
    assert(!accepted(std::string(20,'[')+valid+std::string(20,']')));
    assert(!accepted(changed("\"height\":480","\"height\":240")));
    assert(!accepted(changed("\"version\":1","\"version\":2")));
    assert(!accepted(changed("RGB565_BE","RGB565_LE")));
    assert(!accepted(changed("\"width\":500","\"width\":479")));
    assert(!accepted(changed("\"first_column\":200","\"first_column\":201")));
    assert(!accepted(changed("\"bytes\":288000","\"bytes\":287999")));
    assert(!accepted(changed("b0000001.dat","../file.dat")));
    assert(!accepted(changed("\"columns\":300","\"columns\":4294967295")));
    assert(!accepted(changed("\"valid_height\":480","\"valid_height\":0")));
    assert(!accepted(changed("\"bytes\":288000","\"bytes\":4294967296")));
    std::string large=R"({"format":"indicator-galaxy","version":1,"width":480,"height":480,
      "pixel_format":"RGB565_BE","layout":"column-major","bands":[
      {"width":6000000,"valid_height":480,"chunks":[
      {"file":"b0000000.dat","first_column":0,"columns":3000000,"bytes":2880000000},
      {"file":"b0000001.dat","first_column":3000000,"columns":3000000,"bytes":2880000000}]}]})";
    assert(accepted(large)); // 5.76 GB total, with each file below FAT32's limit.
    if(argc>1) {
        std::ifstream file(argv[1]);assert(file);std::stringstream body;body<<file.rdbuf();
        assert(accepted(body.str()));
    }
    puts("Firmware manifest parser: compatible fixture and malformed-dataset rejection passed");
}
