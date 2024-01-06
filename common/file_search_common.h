
#ifndef FILE_SEARCH_COMMON
#define FILE_SEARCH_COMMON

#define FILEMGR_SEARCH			('S'|('R'<<8)|('C'<<16)|('H'<<24))

enum FILE_SEARCH_EVENT
{
	FILE_MGR_SEARCH_SEARCH,
	FILE_MGR_SEARCH_STOP,
	FILE_MGR_SEARCH_FOUND,
	FILE_MGR_SEARCH_OVER
};

#endif