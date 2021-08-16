// buffer.cpp
#include <iostream>
#include <sstream>
#include <stdlib.h>
#include "buffer.hpp"
#include "utils.hpp"
#include "nlohmann/json.hpp"

using json = nlohmann::json;
using opendfx::Buffer;

Buffer::Buffer(const std::string &name, std::string &parentGraphId) : name(name), parentGraphId(parentGraphId)  {
	utils::setID(id, strid);
	linkRefCount = 0;
}

Buffer::Buffer(const std::string &name, std::string &parentGraphId, const std::string &strid) : name(name), parentGraphId(parentGraphId), strid(strid) {
	linkRefCount = 0;
}

std::string Buffer::info() const {
	return "Buffer name: " + name + "_" + strid;
}

std::string Buffer::getName() const {
	return name;
}

int Buffer::addLinkRefCount(){
	this->linkRefCount ++;
	return 0;
}

int Buffer::subsLinkRefCount(){
	this->linkRefCount --;
	return 0;
}

int Buffer::getLinkRefCount(){
	return this->linkRefCount;
}

std::string Buffer::toJson(bool withDetail){
	json document;
	document["id"]      = strid;
	if(withDetail){
		document["linkRefCount"] = linkRefCount;
		document["deleteFlag"] = deleteFlag;
		document["parentGraphId"]    = parentGraphId;
	}
	document["name"]    = name;
	document["bSize"]    = bSize;
	document["bType"]    = bType;
	std::stringstream jsonStream;
	jsonStream << document.dump(true);

	return jsonStream.str();
}

int Buffer::setDeleteFlag(bool deleteFlag){
	this->deleteFlag = deleteFlag;
	return 0;
}

bool Buffer::getDeleteFlag() const{
	return this->deleteFlag;
}
