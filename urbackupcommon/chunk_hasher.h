#pragma once

#include "../Interface/File.h"
#include <string>

class INotEnoughSpaceCallback
{
public:
	virtual bool handle_not_enough_space(const std::string &path)=0;
};

std::string build_chunk_hashs(IFile *f, IFile *hashoutput, INotEnoughSpaceCallback *cb,
	bool ret_sha2, IFile *copy, bool modify_inplace, int64* inplace_written=NULL,
	IFile* hashinput=NULL, bool show_pc=false);

bool writeRepeatFreeSpace(IFile *f, const char *buf, size_t bsize, INotEnoughSpaceCallback *cb);

bool writeFileRepeatTries(IFile *f, const char *buf, size_t bsize);
