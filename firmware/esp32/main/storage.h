#pragma once
#include <stdint.h>
#include <string>
#include <vector>
struct Chunk { std::string file;uint32_t first,columns,bytes; };
struct Band {uint32_t width,valid_height;std::vector<Chunk> chunks;};
struct Dataset {std::string name,title,credit;std::vector<Band> bands;};
bool parse_dataset(const char* json,Dataset& dataset);
void init_manifest_allocator();
namespace storage {
void init();
bool hello();
bool list(std::vector<std::string>& names);
bool load(const std::string& name,Dataset& dataset);
bool columns(const Dataset& dataset,size_t band,uint32_t start,uint16_t count,uint8_t* out);
const char* error();
}
