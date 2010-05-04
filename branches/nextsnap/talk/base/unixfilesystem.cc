/*
 * libjingle
 * Copyright 2004--2006, Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "talk/base/unixfilesystem.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef OSX
#include <Carbon/Carbon.h>
#include <IOKit/IOCFBundle.h>
#include <sys/statvfs.h>
#include "talk/base/macutils.h"
#endif  // OSX

#if defined(POSIX) && !defined(OSX)
#include <sys/types.h>
#ifdef ANDROID
#include <sys/statfs.h>
#else
#include <sys/statvfs.h>
#endif  // ANDROID
#include <pwd.h>
#include <stdio.h>
#include <unistd.h>
#endif  // POSIX && !OSX

#include "talk/base/fileutils.h"
#include "talk/base/pathutils.h"
#include "talk/base/stream.h"
#include "talk/base/stringutils.h"

#ifdef ANDROID
namespace {
// Android does not have a concept of a single temp dir shared by all
// because resource are scarse on a phone. Instead each app gets some
// space on the sdcard under a path that is given at runtime by the
// system.
// The disk allocation feature is still work in progress so currently
// we return a hardcoded a path on the sdcard. In the future, we
// should do a JNI call to get that info from the context.
// TODO: Replace hardcoded path with a query to the Context
// object to get the equivalents of '/tmp' and '~/.'

// @return the folder for libjingle. Some extra path (typically
// Google/<app name>) will be added.
const char* GetAndroidAppDataFolder() {
  return "/sdcard";
}

// @return the tmp folder to be used. Some extra path will be added to
// that base folder.
const char* GetAndroidTempFolder() {
  return "/sdcard";
}

}  // anonymous namespace
#endif

namespace talk_base {

std::string UnixFilesystem::app_temp_path_;

bool UnixFilesystem::CreateFolder(const Pathname &path) {
  std::string pathname(path.pathname());
  int len = pathname.length();
  if ((len == 0) || (pathname[len - 1] != '/'))
    return false;

  struct stat st;
  int res = ::stat(pathname.c_str(), &st);
  if (res == 0) {
    // Something exists at this location, check if it is a directory
    return S_ISDIR(st.st_mode) != 0;
  } else if (errno != ENOENT) {
    // Unexpected error
    return false;
  }

  // Directory doesn't exist, look up one directory level
  do {
    --len;
  } while ((len > 0) && (pathname[len - 1] != '/'));

  if (!CreateFolder(Pathname(pathname.substr(0, len)))) {
    return false;
  }

  LOG(LS_INFO) << "Creating folder: " << pathname;
  return (0 == ::mkdir(pathname.c_str(), 0755));
}

FileStream *UnixFilesystem::OpenFile(const Pathname &filename,
                                     const std::string &mode) {
  FileStream *fs = new FileStream();
  if (fs && !fs->Open(filename.pathname().c_str(), mode.c_str())) {
    delete fs;
    fs = NULL;
  }
  return fs;
}

bool UnixFilesystem::DeleteFile(const Pathname &filename) {
  LOG(LS_INFO) << "Deleting file:" << filename.pathname();

  if (!IsFile(filename)) {
    ASSERT(IsFile(filename));
    return false;
  }
  return ::unlink(filename.pathname().c_str()) == 0;
}

bool UnixFilesystem::DeleteEmptyFolder(const Pathname &folder) {
  LOG(LS_INFO) << "Deleting folder" << folder.pathname();

  if (!IsFolder(folder)) {
    ASSERT(IsFolder(folder));
    return false;
  }
  std::string no_slash(folder.pathname(), 0, folder.pathname().length()-1);
  return ::rmdir(no_slash.c_str()) == 0;
}

bool UnixFilesystem::GetTemporaryFolder(Pathname &pathname, bool create,
                                        const std::string *append) {
#ifdef OSX
  FSRef fr;
  if (0 != FSFindFolder(kOnAppropriateDisk, kTemporaryFolderType,
                        kCreateFolder, &fr))
    return false;
  unsigned char buffer[NAME_MAX+1];
  if (0 != FSRefMakePath(&fr, buffer, ARRAY_SIZE(buffer)))
    return false;
  pathname.SetPathname(reinterpret_cast<char*>(buffer), "");
#elif defined(ANDROID)
  pathname.SetPathname(GetAndroidTempFolder(), "");
#else  // !OSX && !ANDROID
  if (const char* tmpdir = getenv("TMPDIR")) {
    pathname.SetPathname(tmpdir, "");
  } else if (const char* tmp = getenv("TMP")) {
    pathname.SetPathname(tmp, "");
  } else {
#ifdef P_tmpdir
    pathname.SetPathname(P_tmpdir, "");
#else  // !P_tmpdir
    pathname.SetPathname("/tmp/", "");
#endif  // !P_tmpdir
  }
#endif  // !OSX && !ANDROID
  if (append) {
    ASSERT(!append->empty());
    pathname.AppendFolder(*append);
  }
  return !create || CreateFolder(pathname);
}

std::string UnixFilesystem::TempFilename(const Pathname &dir,
                                         const std::string &prefix) {
  int len = dir.pathname().size() + prefix.size() + 2 + 6;
  char *tempname = new char[len];

  snprintf(tempname, len, "%s/%sXXXXXX", dir.pathname().c_str(),
           prefix.c_str());
  int fd = ::mkstemp(tempname);
  if (fd != -1)
    ::close(fd);
  std::string ret(tempname);
  delete[] tempname;

  return ret;
}

bool UnixFilesystem::MoveFile(const Pathname &old_path,
                              const Pathname &new_path) {
  if (!IsFile(old_path)) {
    ASSERT(IsFile(old_path));
    return false;
  }
  LOG(LS_VERBOSE) << "Moving " << old_path.pathname()
                  << " to " << new_path.pathname();
  if (rename(old_path.pathname().c_str(), new_path.pathname().c_str()) != 0) {
    if (errno != EXDEV)
      return false;
    if (!CopyFile(old_path, new_path))
      return false;
    if (!DeleteFile(old_path))
      return false;
  }
  return true;
}

bool UnixFilesystem::MoveFolder(const Pathname &old_path,
                                const Pathname &new_path) {
  if (!IsFolder(old_path)) {
    ASSERT(IsFolder(old_path));
    return false;
  }
  LOG(LS_VERBOSE) << "Moving " << old_path.pathname()
                  << " to " << new_path.pathname();
  if (rename(old_path.pathname().c_str(), new_path.pathname().c_str()) != 0) {
    if (errno != EXDEV)
      return false;
    if (!CopyFolder(old_path, new_path))
      return false;
    if (!DeleteFolderAndContents(old_path))
      return false;
  }
  return true;
}

bool UnixFilesystem::IsFolder(const Pathname &path) {
  struct stat st;
  if (stat(path.pathname().c_str(), &st) < 0)
    return false;
  return S_ISDIR(st.st_mode);
}

bool UnixFilesystem::CopyFile(const Pathname &old_path,
                              const Pathname &new_path) {
  LOG(LS_VERBOSE) << "Copying " << old_path.pathname()
                  << " to " << new_path.pathname();
  char buf[256];
  size_t len;

  StreamInterface *source = OpenFile(old_path, "rb");
  if (!source)
    return false;

  StreamInterface *dest = OpenFile(new_path, "wb");
  if (!dest) {
    delete source;
    return false;
  }

  while (source->Read(buf, sizeof(buf), &len, NULL) == SR_SUCCESS)
    dest->Write(buf, len, NULL, NULL);

  delete source;
  delete dest;
  return true;
}

bool UnixFilesystem::IsTemporaryPath(const Pathname& pathname) {
  const char* const kTempPrefixes[] = {
#ifdef ANDROID
    GetAndroidTempFolder()
#else
    "/tmp/", "/var/tmp/",
#ifdef OSX
    "/private/tmp/", "/private/var/tmp/", "/private/var/folders/",
#endif  // OSX
#endif  // ANDROID
  };
  for (size_t i = 0; i < ARRAY_SIZE(kTempPrefixes); ++i) {
    if (0 == strncmp(pathname.pathname().c_str(), kTempPrefixes[i],
                     strlen(kTempPrefixes[i])))
      return true;
  }
  return false;
}

bool UnixFilesystem::IsFile(const Pathname& pathname) {
  struct stat st;
  int res = ::stat(pathname.pathname().c_str(), &st);
  // Treat symlinks, named pipes, etc. all as files.
  return res == 0 && !S_ISDIR(st.st_mode);
}

bool UnixFilesystem::IsAbsent(const Pathname& pathname) {
  struct stat st;
  int res = ::stat(pathname.pathname().c_str(), &st);
  // Note: we specifically maintain ENOTDIR as an error, because that implies
  // that you could not call CreateFolder(pathname).
  return res != 0 && ENOENT == errno;
}

bool UnixFilesystem::GetFileSize(const Pathname& pathname, size_t *size) {
  struct stat st;
  if (::stat(pathname.pathname().c_str(), &st) != 0)
    return false;
  *size = st.st_size;
  return true;
}

bool UnixFilesystem::GetFileTime(const Pathname& path, FileTimeType which,
                                 time_t* time) {
  struct stat st;
  if (::stat(path.pathname().c_str(), &st) != 0)
    return false;
  switch (which) {
  case FTT_CREATED:
    *time = st.st_ctime;
    break;
  case FTT_MODIFIED:
    *time = st.st_mtime;
    break;
  case FTT_ACCESSED:
    *time = st.st_atime;
    break;
  default:
    return false;
  }
  return true;
}

bool UnixFilesystem::GetAppPathname(Pathname* path) {
#ifdef OSX
  ProcessSerialNumber psn = { 0, kCurrentProcess };
  CFDictionaryRef procinfo = ProcessInformationCopyDictionary(&psn,
      kProcessDictionaryIncludeAllInformationMask);
  if (NULL == procinfo)
    return false;
  CFStringRef cfpath = (CFStringRef) CFDictionaryGetValue(procinfo,
      kIOBundleExecutableKey);
  std::string path8;
  bool success = ToUtf8(cfpath, &path8);
  CFRelease(procinfo);
  if (success)
    path->SetPathname(path8);
  return success;
#else
  char buffer[NAME_MAX+1];
  size_t len = readlink("/proc/self/exe", buffer, ARRAY_SIZE(buffer) - 1);
  if (len <= 0)
    return false;
  buffer[len] = '\0';
  path->SetPathname(buffer);
  return true;
#endif  // !OSX && !OS_LINUX && !ANDROID
}

bool UnixFilesystem::GetAppDataFolder(Pathname* path, bool per_user) {
  ASSERT(!organization_name_.empty());
  ASSERT(!application_name_.empty());
  std::string prefix;
#ifdef OSX
  if (per_user) {
    // Use ~/Library/Application Support/<orgname>/<appname>/
    FSRef fr;
    if (0 != FSFindFolder(kUserDomain, kApplicationSupportFolderType,
                          kCreateFolder, &fr))
      return false;
    unsigned char buffer[NAME_MAX+1];
    if (0 != FSRefMakePath(&fr, buffer, ARRAY_SIZE(buffer)))
      return false;
    path->SetPathname(reinterpret_cast<char*>(buffer), "");
  } else {
    // TODO
    return false;
  }
#elif defined(ANDROID)
  // TODO: Check if the new disk allocation mechanism works
  // per-user and we don't have the per_user distinction.
  path->SetPathname(GetAndroidAppDataFolder(), "");
#elif OS_LINUX  // && !OSX
  if (per_user) {
    // Use ~/.<orgname>/<appname>/
    if (const char* dotdir = getenv("DOTDIR")) {
      path->SetPathname(dotdir, "");
    } else if (const char* home = getenv("HOME")) {
      path->SetPathname(home, "");
    } else if (passwd* pw = getpwuid(geteuid())) {
      path->SetPathname(pw->pw_dir, "");
    } else {
      return false;
    }
    prefix = ".";
  } else {
    // TODO: This should be set manually at program startup to a directory based
    // on the app's configuration or commandline.  In the meantime, let's use
    // "/var/cache/<orgname>/<appname>/"
    path->SetPathname("/var/cache/", "");
  }
#endif  // OS_LINUX && !OSX
  path->AppendFolder(prefix + organization_name_);
  path->AppendFolder(application_name_);
  return CreateFolder(*path);
}

bool UnixFilesystem::GetAppTempFolder(Pathname* path) {
  ASSERT(!application_name_.empty());
  // TODO: Consider whether we are worried about thread safety.
  if (!app_temp_path_.empty()) {
    path->SetPathname(app_temp_path_);
    return true;
  }

  // Create a random directory as /tmp/<appname>-<pid>-<timestamp>
  char buffer[128];
  sprintfn(buffer, ARRAY_SIZE(buffer), "-%d-%d",
           static_cast<int>(getpid()),
           static_cast<int>(time(0)));
  std::string folder(application_name_);
  folder.append(buffer);
  if (!GetTemporaryFolder(*path, true, &folder))
    return false;

  app_temp_path_ = path->pathname();
  // TODO: atexit(DeleteFolderAndContents(app_temp_path_));
  return true;
}

bool UnixFilesystem::GetDiskFreeSpace(const Pathname& path, int64 *freebytes) {
  ASSERT(NULL != freebytes);
  // TODO: Consider making relative paths absolute using cwd.
  // TODO: When popping off a symlink, push back on the components of the
  // symlink, so we don't jump out of the target disk inadvertently.
  Pathname existing_path(path.folder(), "");
  while (!existing_path.folder().empty() && IsAbsent(existing_path)) {
    existing_path.SetFolder(existing_path.parent_folder());
  }
#ifdef ANDROID
  struct statfs fs;
  memset(&fs, 0, sizeof(fs));
  if (0 != statfs(existing_path.pathname().c_str(), &fs))
    return false;
#else
  struct statvfs vfs;
  memset(&vfs, 0, sizeof(vfs));
  if (0 != statvfs(existing_path.pathname().c_str(), &vfs))
    return false;
#endif  // ANDROID
#ifdef OS_LINUX
  *freebytes = static_cast<int64>(vfs.f_bsize) * vfs.f_bavail;
#elif defined(OSX)
  *freebytes = static_cast<int64>(vfs.f_frsize) * vfs.f_bavail;
#elif defined(ANDROID)
  *freebytes = static_cast<int64>(fs.f_bsize) * fs.f_bavail;
#endif

  return true;
}

Pathname UnixFilesystem::GetCurrentDirectory() {
  Pathname cwd;
#if defined(LINUX) || defined(OSX)
  // Both Linux and Mac supported malloc()'ing the string themselves, although
  // that is not required by POSIX.
  char *path = getcwd(NULL, 0);
#elif defined(ANDROID)
  // Android requires the buffer to be allocated before getcwd is called.
  char buffer[PATH_MAX];
  char *path = getcwd(buffer, PATH_MAX);
#else
#error GetCurrentDirectory() not implemented on this platform
#endif

  if (!path) {
    LOG_ERR(LS_ERROR) << "getcwd() failed";
    return cwd;  // returns empty pathname
  }
  cwd.SetFolder(std::string(path));

#if defined(LINUX) || defined(OSX)
  free(path);
#endif
  return cwd;
}

}  // namespace talk_base