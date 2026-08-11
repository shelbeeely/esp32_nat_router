#pragma once

/* microStore::FileSystem adapter over the router's existing on-flash FATFS
 * mount (esp_vfs_fat_spiflash_mount_rw_wl, mounted at MOUNT_PATH="/data" in
 * main/esp32_nat_router.c for console history) -- plain POSIX stdio, no SD
 * card and no separate LittleFS partition.
 *
 * microStore ships a UniversalFileSystem adapter that looks like it would do
 * this job, but on ESP32 it resolves to PosixFileSystem, which internally
 * calls Arduino's `LittleFS.begin(true, basepath)` -- a *different* flash
 * filesystem needing its own partition (default label "spiffs"), not a path
 * prefix into whatever's already mounted. Reusing the "storage" FAT
 * partition for both would either fail to mount or trigger LittleFS's
 * format-on-fail path against a FAT-formatted partition. Writing directly
 * against the existing mount's POSIX VFS sidesteps that partition-table
 * question entirely for this first pass.
 */

#include <microStore/File.h>
#include <microStore/FileSystem.h>

#include <stdio.h>

namespace reticulum_bridge {

class FatFsFileImpl : public microStore::FileImpl {
public:
    FatFsFileImpl(FILE *fp, std::string name) : _fp(fp), _name(std::move(name)) {}
    virtual ~FatFsFileImpl() { close(); }

    virtual const char *name() const override { return _name.c_str(); }
    virtual size_t size() const override;
    virtual void close() override;

    virtual int read() override;
    virtual size_t write(uint8_t ch) override;
    virtual size_t read(uint8_t *buffer, size_t size) override;
    virtual size_t write(const uint8_t *buffer, size_t size) override;

    virtual int available() override;
    virtual int peek() override;
    virtual size_t tell() override;
    virtual long seek(uint32_t pos, microStore::SeekMode mode) override;
    virtual void flush() override;

    virtual bool isValid() const override { return _fp != nullptr; }

private:
    FILE *_fp;
    std::string _name;
};

class FatFsFileSystemImpl : public microStore::FileSystemImpl {
public:
    explicit FatFsFileSystemImpl(std::string basepath) : _basepath(std::move(basepath)) {}
    virtual ~FatFsFileSystemImpl() = default;

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

class FatFsFileSystem : public microStore::FileSystem {
public:
    explicit FatFsFileSystem(const char *basepath) : microStore::FileSystem(new FatFsFileSystemImpl(basepath)) {}
};

} // namespace reticulum_bridge
