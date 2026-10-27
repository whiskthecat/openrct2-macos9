// Directory watching is not available on Mac OS 9.
#include <openrct2/core/FileWatcher.h>

FileWatcher::FileWatcher(u8string_view)
{
}

FileWatcher::~FileWatcher()
{
}

void FileWatcher::WatchDirectory()
{
}
