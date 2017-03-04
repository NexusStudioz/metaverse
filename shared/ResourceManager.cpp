/*=====================================================================
ResourceManager.cpp
-------------------
Copyright Glare Technologies Limited 2016 -
Generated at 2016-01-12 12:22:34 +1300
=====================================================================*/
#include "ResourceManager.h"


#include <ConPrint.h>
#include <StringUtils.h>
#include <FileUtils.h>
#include <Exception.h>


ResourceManager::ResourceManager(const std::string& base_resource_dir_)
:	base_resource_dir(base_resource_dir_)
{
}


ResourceManager::~ResourceManager()
{
}


static std::string sanitiseString(const std::string& s)
{
	std::string res = s;
	for(size_t i=0; i<s.size(); ++i)
		if(!::isAlphaNumeric(s[i]))
			res[i] = '_';
	return res;
}


const std::string ResourceManager::URLForNameAndExtensionAndHash(const std::string& name, const std::string& extension, uint64 hash)
{
	return sanitiseString(name) + "_" + toString(hash) + "." + extension;
}


const std::string ResourceManager::URLForPathAndHash(const std::string& path, uint64 hash)
{
	const std::string filename = FileUtils::getFilename(path);

	const std::string extension = ::getExtension(filename);
	
	return sanitiseString(filename) + "_" + toString(hash) + "." + extension;
}


bool ResourceManager::isValidURL(const std::string& URL)
{
	for(size_t i=0; i<URL.size(); ++i)
		if(!(::isAlphaNumeric(URL[i]) || URL[i] == '_' || URL[i] == '.'))
			return false;
	return true;
}


const std::string ResourceManager::pathForURL(const std::string& URL)
{
	if(!isValidURL(URL))
		throw Indigo::Exception("Invalid URL '" + URL + "'");
	return base_resource_dir + "/" + URL;
}
