#pragma once

/* microStore::FileSystem adapter over the X4's SD card (FreeInk SDK's
 * SDCardManager singleton, third_party/freeink-sdk submodule), used as
 * Reticulum's storage backend when a card is present -- much more room than
 * the internal flash mount FatFsFileSystem falls back to (identities, the
 * path table, and RNS_PERSIST_* caches all live under one basepath
 * directory here, same layout convention as FatFsFileSystem).
 *
 * SDCardManager is a singleton (SdMan / SDCardManager::getInstance()) shared
 * with nothing else in this firmware today -- only microreticulum vendors
 * SDCardManager.cpp, so there's no duplicate-definition risk from another
 * component also compiling it in.
 */

/* <string> must come before the microStore headers below -- they use
 * std::string (e.g. debugString()) without including <string> themselves. */
#include <string>

#include <microStore/File.h>
#include <microStore/FileSystem.h>

#include <SDCardManager.h>

namespace reticulum_bridge {

class SdCardFileImpl : public microStore::FileImpl {
public:
    /* Constructs (and opens) _file in place via the mem-initializer-list --
     * SdFat's FsFile has both its copy and move constructors deleted (file
     * handles are non-transferable), so this can never take an already-open
     * FsFile by value/rvalue. C++17 guaranteed copy elision means the
     * SdMan.open(...) prvalue is materialized directly into _file, no
     * copy/move ctor involved. */
    SdCardFileImpl(const char *path, oflag_t oflag, std::string name)
        : _file(SdMan.open(path, oflag)), _name(std::move(name)) {}
    virtual ~SdCardFileImpl() { close(); }

    virtual const char *name() const override { return _name.c_str(); }
    virtual size_t size() const override { return _file.isOpen() ? (size_t)_file.fileSize() : 0; }
    virtual void close() override {
        if (_file.isOpen())
            _file.close();
    }

    virtual int read() override { return _file.isOpen() ? _file.read() : -1; }
    virtual size_t write(uint8_t ch) override { return _file.isOpen() ? _file.write(ch) : 0; }
    virtual size_t read(uint8_t *buffer, size_t size) override {
        if (!_file.isOpen())
            return 0;
        int n = _file.read(buffer, size);
        return n > 0 ? (size_t)n : 0;
    }
    virtual size_t write(const uint8_t *buffer, size_t size) override {
        return _file.isOpen() ? _file.write(buffer, size) : 0;
    }

    virtual int available() override { return _file.isOpen() ? _file.available() : 0; }
    virtual int peek() override { return _file.isOpen() ? _file.peek() : -1; }
    virtual size_t tell() override { return _file.isOpen() ? (size_t)_file.curPosition() : 0; }
    virtual long seek(uint32_t pos, microStore::SeekMode mode) override;
    virtual void flush() override {
        if (_file.isOpen())
            _file.sync();
    }

    virtual bool isValid() const override { return _file.isOpen(); }

private:
    FsFile _file;
    std::string _name;
};

class SdCardFileSystemImpl : public microStore::FileSystemImpl {
public:
    explicit SdCardFileSystemImpl(std::string basepath) : _basepath(std::move(basepath)) {}
    virtual ~SdCardFileSystemImpl() = default;

    virtual bool init(bool reformatOnFail = true) override;

    virtual microStore::File open(const char *path, microStore::File::Mode mode, const bool create = false) override;
    virtual bool exists(const char *path) override;
    virtual bool remove(const char *path) override;
    virtual bool rename(const char *from_path, const char *to_path) override;
    virtual bool mkdir(const char *path) override;
    virtual bool rmdir(const char *path) override;
    virtual bool isDirectory(const char *path) override;
    virtual std::list<std::string> listDirectory(const char *path, Callbacks::DirectoryListing callback) override;
    virtual size_t storageSize() override;
    virtual size_t storageAvailable() override;

private:
    std::string fullPath(const char *path) const;

    std::string _basepath;
};

class SdCardFileSystem : public microStore::FileSystem {
public:
    explicit SdCardFileSystem(const char *basepath) : microStore::FileSystem(new SdCardFileSystemImpl(basepath)) {}
};

} // namespace reticulum_bridge
