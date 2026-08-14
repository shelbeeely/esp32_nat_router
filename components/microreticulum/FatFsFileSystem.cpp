#include "FatFsFileSystem.h"

#include <sys/stat.h>
#include <dirent.h>
#include <string.h>

namespace reticulum_bridge {

/* ---- FatFsFileImpl ---- */

size_t FatFsFileImpl::size() const {
    if (!_fp)
        return 0;
    long cur = ftell(_fp);
    fseek(_fp, 0, SEEK_END);
    long end = ftell(_fp);
    fseek(_fp, cur, SEEK_SET);
    return end > 0 ? (size_t)end : 0;
}

void FatFsFileImpl::close() {
    if (_fp) {
        fclose(_fp);
        _fp = nullptr;
    }
}

int FatFsFileImpl::read() {
    if (!_fp)
        return -1;
    return fgetc(_fp);
}

size_t FatFsFileImpl::write(uint8_t ch) {
    if (!_fp)
        return 0;
    return fwrite(&ch, 1, 1, _fp);
}

size_t FatFsFileImpl::read(uint8_t *buffer, size_t size) {
    if (!_fp)
        return 0;
    return fread(buffer, 1, size, _fp);
}

size_t FatFsFileImpl::write(const uint8_t *buffer, size_t size) {
    if (!_fp)
        return 0;
    return fwrite(buffer, 1, size, _fp);
}

int FatFsFileImpl::available() {
    if (!_fp)
        return 0;
    long cur = ftell(_fp);
    long total = (long)size();
    return (int)(total - cur);
}

int FatFsFileImpl::peek() {
    if (!_fp)
        return -1;
    int ch = fgetc(_fp);
    if (ch != EOF)
        ungetc(ch, _fp);
    return ch;
}

size_t FatFsFileImpl::tell() {
    if (!_fp)
        return 0;
    long pos = ftell(_fp);
    return pos > 0 ? (size_t)pos : 0;
}

long FatFsFileImpl::seek(uint32_t pos, microStore::SeekMode mode) {
    if (!_fp)
        return -1;
    int whence = (mode == microStore::SeekModeCur) ? SEEK_CUR : (mode == microStore::SeekModeEnd) ? SEEK_END : SEEK_SET;
    return fseek(_fp, (long)pos, whence);
}

void FatFsFileImpl::flush() {
    if (_fp)
        fflush(_fp);
}

/* ---- FatFsFileSystemImpl ---- */

std::string FatFsFileSystemImpl::fullPath(const char *path) const {
    std::string p(path ? path : "");
    /* RNS/microStore paths are typically "." or "./name" relative to the
     * filesystem root; strip a leading "./" and join onto _basepath. */
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

bool FatFsFileSystemImpl::init(bool reformatOnFail) {
    (void)reformatOnFail; /* nothing to reformat -- this rides the router's
                            * existing FATFS mount, which owns its own
                            * format-on-first-mount handling. */
    struct stat st;
    if (stat(_basepath.c_str(), &st) == 0)
        return S_ISDIR(st.st_mode);
    return ::mkdir(_basepath.c_str(), 0775) == 0;
}

microStore::File FatFsFileSystemImpl::open(const char *path, microStore::File::Mode mode, const bool create) {
    std::string full = fullPath(path);
    const char *fmode;
    switch (mode) {
        case microStore::File::ModeWrite:      fmode = "wb"; break;
        case microStore::File::ModeAppend:     fmode = "ab"; break;
        case microStore::File::ModeReadWrite:  fmode = create ? "w+b" : "r+b"; break;
        case microStore::File::ModeReadAppend: fmode = "a+b"; break;
        case microStore::File::ModeRead:
        default:
            fmode = "rb";
            break;
    }

    FILE *fp = fopen(full.c_str(), fmode);
    if (!fp && mode == microStore::File::ModeRead && create) {
        /* Emulate "open for read, creating an empty file if missing" --
         * plain fopen("rb") can't create, so create-then-reopen. */
        FILE *touch = fopen(full.c_str(), "wb");
        if (touch) {
            fclose(touch);
            fp = fopen(full.c_str(), "rb");
        }
    }
    if (!fp)
        return microStore::File();

    return microStore::File(new FatFsFileImpl(fp, path ? path : ""));
}

bool FatFsFileSystemImpl::exists(const char *path) {
    struct stat st;
    return stat(fullPath(path).c_str(), &st) == 0;
}

bool FatFsFileSystemImpl::remove(const char *path) {
    return ::remove(fullPath(path).c_str()) == 0;
}

bool FatFsFileSystemImpl::rename(const char *from_path, const char *to_path) {
    return ::rename(fullPath(from_path).c_str(), fullPath(to_path).c_str()) == 0;
}

bool FatFsFileSystemImpl::mkdir(const char *path) {
    std::string full = fullPath(path);
    struct stat st;
    if (stat(full.c_str(), &st) == 0)
        return S_ISDIR(st.st_mode);
    return ::mkdir(full.c_str(), 0775) == 0;
}

bool FatFsFileSystemImpl::rmdir(const char *path) {
    return ::rmdir(fullPath(path).c_str()) == 0;
}

bool FatFsFileSystemImpl::isDirectory(const char *path) {
    struct stat st;
    if (stat(fullPath(path).c_str(), &st) != 0)
        return false;
    return S_ISDIR(st.st_mode);
}

std::list<std::string> FatFsFileSystemImpl::listDirectory(const char *path, Callbacks::DirectoryListing callback) {
    std::list<std::string> names;
    DIR *dir = opendir(fullPath(path).c_str());
    if (!dir)
        return names;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        names.push_back(entry->d_name);
        if (callback)
            callback(entry->d_name);
    }
    closedir(dir);
    return names;
}

size_t FatFsFileSystemImpl::storageSize() {
    /* Not wired to the wear-levelled partition's real capacity -- only used
     * for informational logging today, not a correctness path. */
    return 0;
}

size_t FatFsFileSystemImpl::storageAvailable() {
    return 0;
}

} // namespace reticulum_bridge
