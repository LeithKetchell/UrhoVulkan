// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#include "../Precompiled.h"

#include "../IO/ZarArchive.h"
#include "../IO/Compression.h"
#include "../IO/File.h"
#include "../IO/FileSystem.h"
#include "../IO/Log.h"
#include "../IO/VectorBuffer.h"
#include "../Core/Context.h"

#include <LZ4/lz4.h>
#include <cstring>

#include "../DebugNew.h"

namespace Urho3D
{

// Format constants
static const unsigned char ENTRY_MAGIC[4] = {'Z', 'A', 'R', 0x01};
static const unsigned char DIR_MAGIC[4]   = {'Z', 'D', 'I', 'R'};
static const unsigned char FOOTER_MAGIC[4] = {'Z', 'E', 'N', 'D'};
static const unsigned char TAIL_SENTINEL[2] = {'Z', 'R'};
static const unsigned short FORMAT_VERSION = 1;
static const unsigned FOOTER_SIZE = 24;

// CRC32 (standard polynomial)
static unsigned Crc32(const void* data, unsigned long long size)
{
    static unsigned table[256];
    static bool tableBuilt = false;
    if (!tableBuilt)
    {
        for (unsigned i = 0; i < 256; ++i)
        {
            unsigned c = i;
            for (int j = 0; j < 8; ++j)
                c = (c >> 1) ^ (0xEDB88320 & (-(c & 1)));
            table[i] = c;
        }
        tableBuilt = true;
    }

    unsigned crc = 0xFFFFFFFF;
    const auto* p = static_cast<const unsigned char*>(data);
    for (unsigned long long i = 0; i < size; ++i)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}

// Helper: write raw bytes
static bool WriteBytes(File& f, const void* data, unsigned size)
{
    return f.Write(data, size) == size;
}

// Helper: write little-endian integers
static bool WriteU16(File& f, unsigned short v)
{
    unsigned char buf[2] = {(unsigned char)(v & 0xFF), (unsigned char)(v >> 8)};
    return WriteBytes(f, buf, 2);
}

static bool WriteU32(File& f, unsigned v)
{
    unsigned char buf[4];
    for (int i = 0; i < 4; ++i) { buf[i] = (unsigned char)(v & 0xFF); v >>= 8; }
    return WriteBytes(f, buf, 4);
}

static bool WriteU64(File& f, unsigned long long v)
{
    unsigned char buf[8];
    for (int i = 0; i < 8; ++i) { buf[i] = (unsigned char)(v & 0xFF); v >>= 8; }
    return WriteBytes(f, buf, 8);
}

static bool WriteU8(File& f, unsigned char v)
{
    return WriteBytes(f, &v, 1);
}

// Helper: read little-endian integers
static unsigned short ReadU16(File& f)
{
    unsigned char buf[2];
    f.Read(buf, 2);
    return (unsigned short)buf[0] | ((unsigned short)buf[1] << 8);
}

static unsigned ReadU32(File& f)
{
    unsigned char buf[4];
    f.Read(buf, 4);
    return (unsigned)buf[0] | ((unsigned)buf[1] << 8) | ((unsigned)buf[2] << 16) | ((unsigned)buf[3] << 24);
}

static unsigned long long ReadU64(File& f)
{
    unsigned char buf[8];
    f.Read(buf, 8);
    unsigned long long v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | buf[i];
    return v;
}

static unsigned char ReadU8(File& f)
{
    unsigned char v;
    f.Read(&v, 1);
    return v;
}

// ============================================================================
// ZarWriter
// ============================================================================

ZarWriter::ZarWriter(Context* context) :
    Object(context)
{
}

ZarWriter::~ZarWriter()
{
    if (open_)
        Close();
}

bool ZarWriter::Open(const String& path)
{
    if (open_)
        Close();

    outputPath_ = path;
    tempPath_ = path + ".tmp";

    tempFile_ = new File(context_, tempPath_, FILE_WRITE);
    if (!tempFile_->IsOpen())
    {
        URHO3D_LOGERROR("ZarWriter: Failed to open temp file " + tempPath_);
        tempFile_.Reset();
        return false;
    }

    entries_.Clear();
    open_ = true;
    return true;
}

bool ZarWriter::AddFile(const String& entryPath, const void* data, unsigned long long size,
                         ZarMethod method, unsigned long long modTime)
{
    if (!open_ || !tempFile_)
        return false;

    ZarEntry entry;
    entry.path = entryPath;
    entry.offset = 0;  // will be fixed up in Close()
    entry.origSize = size;
    entry.crc32 = Crc32(data, size);
    entry.method = method;
    entry.modTime = modTime;

    // Compress if requested
    const void* writeData = data;
    unsigned long long writeSize = size;
    unsigned char* compBuf = nullptr;

    if (method == ZAR_LZ4 && size > 0)
    {
        int bound = LZ4_compressBound((int)Min(size, (unsigned long long)0x7FFFFFFF));
        if (bound > 0)
        {
            compBuf = new unsigned char[bound];
            int compSize = LZ4_compress_default(
                static_cast<const char*>(data), reinterpret_cast<char*>(compBuf),
                (int)size, bound);
            if (compSize > 0 && (unsigned long long)compSize < size)
            {
                writeData = compBuf;
                writeSize = (unsigned long long)compSize;
            }
            else
                entry.method = ZAR_STORE;
        }
        else
            entry.method = ZAR_STORE;
    }

    entry.compSize = writeSize;

    // Record where this entry starts in the temp file
    entry.offset = (unsigned long long)tempFile_->GetSize();

    // Write entry header + data to temp file
    WriteBytes(*tempFile_, ENTRY_MAGIC, 4);
    WriteU16(*tempFile_, (unsigned short)entryPath.Length());
    WriteBytes(*tempFile_, entryPath.CString(), entryPath.Length());
    WriteU16(*tempFile_, 0);  // flags
    WriteU8(*tempFile_, (unsigned char)entry.method);
    WriteU64(*tempFile_, entry.origSize);
    WriteU64(*tempFile_, entry.compSize);
    WriteU32(*tempFile_, entry.crc32);
    WriteU64(*tempFile_, entry.modTime);
    WriteBytes(*tempFile_, writeData, (unsigned)writeSize);

    delete[] compBuf;

    entries_.Push(entry);
    return true;
}

bool ZarWriter::AddFile(const String& entryPath, File& srcFile, ZarMethod method)
{
    unsigned long long remaining = (unsigned long long)(srcFile.GetSize() - srcFile.GetPosition());
    auto* buf = new unsigned char[(unsigned)remaining];
    srcFile.Read(buf, (int)remaining);

    auto* fs = GetSubsystem<FileSystem>();
    unsigned long long modTime = 0;
    if (fs)
        modTime = (unsigned long long)fs->GetLastModifiedTime(srcFile.GetName());

    bool ok = AddFile(entryPath, buf, remaining, method, modTime);
    delete[] buf;
    return ok;
}

bool ZarWriter::Close()
{
    if (!open_ || !tempFile_)
        return false;

    tempFile_->Close();

    // Calculate directory size so we know the offset adjustment for entries
    // Directory: 4 (ZDIR) + 8 (count) + per-entry (2 + pathLen + 8 + 8 + 8 + 4 + 1 + 8 = 39 + pathLen)
    unsigned long long dirSize = 4 + 8;
    for (unsigned i = 0; i < entries_.Size(); ++i)
        dirSize += 39 + (unsigned long long)entries_[i].path.Length();

    // Entry offsets need to shift by dirSize (directory comes first)
    unsigned long long entryDataOffset = dirSize;

    // Open final output file
    File outFile(context_, outputPath_, FILE_WRITE);
    if (!outFile.IsOpen())
    {
        URHO3D_LOGERROR("ZarWriter: Failed to open output " + outputPath_);
        return false;
    }

    // 1. Write central directory at front
    WriteBytes(outFile, DIR_MAGIC, 4);
    WriteU64(outFile, (unsigned long long)entries_.Size());

    for (unsigned i = 0; i < entries_.Size(); ++i)
    {
        ZarEntry& e = entries_[i];
        // Adjust offset: temp file offset + directory size
        unsigned long long finalOffset = e.offset + entryDataOffset;

        WriteU16(outFile, (unsigned short)e.path.Length());
        WriteBytes(outFile, e.path.CString(), e.path.Length());
        WriteU64(outFile, finalOffset);
        WriteU64(outFile, e.origSize);
        WriteU64(outFile, e.compSize);
        WriteU32(outFile, e.crc32);
        WriteU8(outFile, (unsigned char)e.method);
        WriteU64(outFile, e.modTime);
    }

    // 2. Copy entry data from temp file
    File tempIn(context_, tempPath_, FILE_READ);
    if (!tempIn.IsOpen())
    {
        URHO3D_LOGERROR("ZarWriter: Failed to reopen temp file");
        return false;
    }

    const int COPY_BUF = 1024 * 1024;  // 1MB chunks
    auto* copyBuf = new unsigned char[COPY_BUF];
    for (;;)
    {
        int bytesRead = tempIn.Read(copyBuf, COPY_BUF);
        if (bytesRead <= 0)
            break;
        outFile.Write(copyBuf, bytesRead);
    }
    delete[] copyBuf;
    tempIn.Close();

    // 3. Write footer (24 bytes)
    WriteBytes(outFile, FOOTER_MAGIC, 4);
    WriteU64(outFile, 0ULL);  // dirOffset = 0 (directory is at front)
    WriteU64(outFile, dirSize);
    WriteU16(outFile, FORMAT_VERSION);
    WriteBytes(outFile, TAIL_SENTINEL, 2);

    outFile.Close();

    // Clean up temp file
    auto* fs = GetSubsystem<FileSystem>();
    if (fs)
        fs->Delete(tempPath_);

    tempFile_.Reset();
    open_ = false;

    URHO3D_LOGINFOF("ZarWriter: Wrote %u entries (directory at front)", entries_.Size());
    return true;
}

// ============================================================================
// ZarReader
// ============================================================================

ZarReader::ZarReader(Context* context) :
    Object(context)
{
}

ZarReader::~ZarReader()
{
    if (open_)
        Close();
}

bool ZarReader::Open(const String& path)
{
    if (open_)
        Close();

    file_ = new File(context_, path, FILE_READ);
    if (!file_->IsOpen())
    {
        URHO3D_LOGERROR("ZarReader: Failed to open " + path);
        file_.Reset();
        return false;
    }

    // Try fast path: directory at front (v2 format)
    unsigned char probe[4];
    file_->Read(probe, 4);
    unsigned long long dirOffset = 0;

    if (memcmp(probe, DIR_MAGIC, 4) == 0)
    {
        // Directory is at front — no footer seek needed (HTTP-friendly)
        dirOffset = 0;
        file_->Seek(0);
    }
    else
    {
        // Fall back to footer (v1 tail-end directory)
        long long fileSize = (long long)file_->GetSize();
        if (fileSize < FOOTER_SIZE)
        {
            URHO3D_LOGERROR("ZarReader: File too small for footer");
            file_.Reset();
            return false;
        }

        file_->Seek(fileSize - FOOTER_SIZE);

        unsigned char magic[4];
        file_->Read(magic, 4);
        if (memcmp(magic, FOOTER_MAGIC, 4) != 0)
        {
            URHO3D_LOGERROR("ZarReader: Invalid footer/directory magic");
            file_.Reset();
            return false;
        }

        dirOffset = ReadU64(*file_);
        unsigned long long dirSize = ReadU64(*file_);
        unsigned short version = ReadU16(*file_);
        unsigned char sentinel[2];
        file_->Read(sentinel, 2);

        if (memcmp(sentinel, TAIL_SENTINEL, 2) != 0)
        {
            URHO3D_LOGERROR("ZarReader: Invalid tail sentinel");
            file_.Reset();
            return false;
        }

        if (version > FORMAT_VERSION)
            URHO3D_LOGWARNING("ZarReader: Archive version " + String(version) + " is newer");

        file_->Seek((long long)dirOffset);
    }

    // Read central directory

    unsigned char dirMagic[4];
    file_->Read(dirMagic, 4);
    if (memcmp(dirMagic, DIR_MAGIC, 4) != 0)
    {
        URHO3D_LOGERROR("ZarReader: Invalid directory magic");
        file_.Reset();
        return false;
    }

    unsigned long long entryCount = ReadU64(*file_);
    entries_.Reserve((unsigned)entryCount);
    entryIndex_.Clear();

    for (unsigned long long i = 0; i < entryCount; ++i)
    {
        ZarEntry e;
        unsigned short pathLen = ReadU16(*file_);
        char pathBuf[65536];
        file_->Read(pathBuf, pathLen);
        e.path = String(pathBuf, pathLen);
        e.offset = ReadU64(*file_);
        e.origSize = ReadU64(*file_);
        e.compSize = ReadU64(*file_);
        e.crc32 = ReadU32(*file_);
        e.method = (ZarMethod)ReadU8(*file_);
        e.modTime = ReadU64(*file_);

        entryIndex_[e.path] = entries_.Size();
        entries_.Push(e);
    }

    open_ = true;
    URHO3D_LOGINFOF("ZarReader: Opened %s (%llu entries)", path.CString(), entryCount);
    return true;
}

void ZarReader::Close()
{
    if (file_)
        file_->Close();
    file_.Reset();
    entries_.Clear();
    entryIndex_.Clear();
    open_ = false;
}

Vector<String> ZarReader::GetFileList() const
{
    Vector<String> list;
    list.Reserve(entries_.Size());
    for (unsigned i = 0; i < entries_.Size(); ++i)
        list.Push(entries_[i].path);
    return list;
}

bool ZarReader::HasFile(const String& entryPath) const
{
    return entryIndex_.Contains(entryPath);
}

unsigned long long ZarReader::GetFileSize(const String& entryPath) const
{
    auto it = entryIndex_.Find(entryPath);
    if (it == entryIndex_.End())
        return 0;
    return entries_[it->second_].origSize;
}

const ZarEntry* ZarReader::GetEntry(const String& entryPath) const
{
    auto it = entryIndex_.Find(entryPath);
    if (it == entryIndex_.End())
        return nullptr;
    return &entries_[it->second_];
}

unsigned long long ZarReader::ReadFile(const String& entryPath, void* dest, unsigned long long destSize)
{
    if (!open_ || !file_)
        return 0;

    auto it = entryIndex_.Find(entryPath);
    if (it == entryIndex_.End())
        return 0;

    const ZarEntry& e = entries_[it->second_];
    if (destSize < e.origSize)
    {
        URHO3D_LOGERROR("ZarReader: Buffer too small for " + entryPath);
        return 0;
    }

    // Seek past entry header to data
    // Header: 4 (magic) + 2 (pathLen) + pathLen + 2 (flags) + 1 (method) + 8 (origSize) + 8 (compSize) + 4 (crc32) + 8 (modTime)
    unsigned headerSize = 4 + 2 + e.path.Length() + 2 + 1 + 8 + 8 + 4 + 8;
    file_->Seek((long long)(e.offset + headerSize));

    if (e.method == ZAR_STORE)
    {
        file_->Read(dest, (unsigned)e.origSize);
    }
    else if (e.method == ZAR_LZ4)
    {
        auto* compBuf = new unsigned char[(unsigned)e.compSize];
        file_->Read(compBuf, (unsigned)e.compSize);

        int result = LZ4_decompress_safe(
            reinterpret_cast<const char*>(compBuf),
            static_cast<char*>(dest),
            (int)e.compSize, (int)e.origSize);

        delete[] compBuf;

        if (result < 0)
        {
            URHO3D_LOGERROR("ZarReader: LZ4 decompression failed for " + entryPath);
            return 0;
        }
    }

    // Verify CRC
    unsigned crc = Crc32(dest, e.origSize);
    if (crc != e.crc32)
    {
        URHO3D_LOGWARNING("ZarReader: CRC mismatch for " + entryPath);
    }

    return e.origSize;
}

VectorBuffer ZarReader::ReadFileBuffer(const String& entryPath)
{
    VectorBuffer result;

    auto it = entryIndex_.Find(entryPath);
    if (it == entryIndex_.End())
        return result;

    const ZarEntry& e = entries_[it->second_];
    result.Resize((unsigned)e.origSize);

    unsigned long long read = ReadFile(entryPath, result.GetModifiableData(), e.origSize);
    if (read == 0)
        result.Clear();

    return result;
}

}
