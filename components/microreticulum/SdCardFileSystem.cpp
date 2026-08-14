#include "SdCardFileSystem.h"

#include <string.h>

namespace reticulum_bridge {

long SdCardFileImpl::seek(uint32_t pos, microStore::SeekMode mode) {
    if (!_file.isOpen())
        return -1;
    bool ok;
    switch (mode) {
        case microStore::SeekModeCur: ok = _file.seekCur((int64_t)pos); break;
        case microStore::SeekModeEnd: ok = _file.seekEnd((int64_t)pos); break;
        case microStore::SeekModeSet:
        default:                     ok = _file.seekSet(pos); break;
    }
    return ok ? 0 : -1;
}

std::string SdCardFileSystemImpl::fullPath(const char *path) const {
    std::string p(path ? path : "");
    if (p == ".")
        return _basepath;
    if (p.size() >= 2 && p[0] == '.' && p[1] == '/')
        p = p.substr(2);
    if (!p.empty() && p[0] == '/')
        p = p.substr(1);
    if (p.empty())
        return _basepath;
    return _basepath + "/" + p;
}

bool SdCardFileSystemImpl::init(bool reformatOnFail) {
    (void)reformatOnFail; /* SDCardManager::begin() does its own bring-up; no
                            * reformat path -- see its header. */
    if (!SdMan.begin())
        return false;
    return SdMan.ensureDirectoryExists(_basepath.c_str());
}

microStore::File SdCardFileSystemImpl::open(const char *path, microStore::File::Mode mode, const bool create) {
    std::string full = fullPath(path);
    oflag_t flags;
    switch (mode) {
        case microStore::File::ModeWrite:      flags = O_WRONLY | O_CREAT | O_TRUNC; break;
        case microStore::File::ModeAppend:     flags = O_WRONLY | O_CREAT | O_APPEND; break;
        case microStore::File::ModeReadWrite:  flags = O_RDWR | (create ? O_CREAT : 0); break;
        case microStore::File::ModeReadAppend: flags = O_RDWR | O_CREAT | O_APPEND; break;
        case microStore::File::ModeRead:
        default:
            flags = O_RDONLY | (create ? O_CREAT : 0);
            break;
    }

    SdCardFileImpl *impl = new SdCardFileImpl(full.c_str(), flags, path ? path : "");
    if (!impl->isValid()) {
        delete impl;
        return microStore::File();
    }

    return microStore::File(impl);
}

bool SdCardFileSystemImpl::exists(const char *path) {
    return SdMan.exists(fullPath(path).c_str());
}

bool SdCardFileSystemImpl::remove(const char *path) {
    return SdMan.remove(fullPath(path).c_str());
}

bool SdCardFileSystemImpl::rename(const char *from_path, const char *to_path) {
    return SdMan.rename(fullPath(from_path).c_str(), fullPath(to_path).c_str());
}

bool SdCardFileSystemImpl::mkdir(const char *path) {
    return SdMan.mkdir(fullPath(path).c_str());
}

bool SdCardFileSystemImpl::rmdir(const char *path) {
    return SdMan.rmdir(fullPath(path).c_str());
}

bool SdCardFileSystemImpl::isDirectory(const char *path) {
    FsFile file = SdMan.open(fullPath(path).c_str(), O_RDONLY);
    if (!file.isOpen())
        return false;
    bool isDir = file.isDir();
    file.close();
    return isDir;
}

std::list<std::string> SdCardFileSystemImpl::listDirectory(const char *path, Callbacks::DirectoryListing callback) {
    std::list<std::string> names;

    FsFile dir = SdMan.open(fullPath(path).c_str(), O_RDONLY);
    if (!dir.isOpen())
        return names;

    FsFile entry;
    char nameBuf[256];
    while (entry.openNext(&dir, O_RDONLY)) {
        size_t len = entry.getName(nameBuf, sizeof(nameBuf));
        entry.close();
        if (len == 0 || strcmp(nameBuf, ".") == 0 || strcmp(nameBuf, "..") == 0)
            continue;
        names.push_back(nameBuf);
        if (callback)
            callback(nameBuf);
    }
    dir.close();
    return names;
}

size_t SdCardFileSystemImpl::storageSize() {
    return (size_t)SdMan.sdTotalBytes();
}

size_t SdCardFileSystemImpl::storageAvailable() {
    uint64_t total = SdMan.sdTotalBytes();
    uint64_t used = SdMan.sdUsedBytes();
    return total > used ? (size_t)(total - used) : 0;
}

} // namespace reticulum_bridge
