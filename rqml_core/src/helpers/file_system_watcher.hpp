/*
 * Copyright (C) 2019  Stefan Fabian
 *
 * This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef RQML_FILE_WATCHER_H
#define RQML_FILE_WATCHER_H

#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class QFileSystemWatcher;

namespace rqml
{

/*!
 * Watches files and directories for changes.
 *
 * Built on QFileSystemWatcher, so it works on every platform Qt supports. Change notifications are
 * delivered as Qt signals, so the watcher must be created in, and used from, a thread that runs a Qt
 * event loop. checkForChanges() itself may be called from any thread.
 */
class FileSystemWatcher
{
public:
  FileSystemWatcher();

  ~FileSystemWatcher();

  FileSystemWatcher( const FileSystemWatcher & ) = delete;
  FileSystemWatcher &operator=( const FileSystemWatcher & ) = delete;

  bool isValid() const;

  bool addWatch( const std::string &path );

  void removeWatch( const std::string &path );

  void removeAllWatches();

  //! Returns true if one of the watched paths has changed since the last call to checkForChanges().
  bool checkForChanges() const;

private:
  struct Target {
    QString path; //!< Canonical path (symlinks resolved, '/' separators)
    bool is_directory;
  };

  struct FileState {
    bool exists = false;
    std::uintmax_t size = 0;
    std::filesystem::file_time_type modified{};

    bool operator==( const FileState &other ) const
    {
      return exists == other.exists && size == other.size && modified == other.modified;
    }
    bool operator!=( const FileState &other ) const { return !( *this == other ); }
  };

  static FileState readFileState( const QString &path );

  //! Recomputes the set of watched directories and files from targets_ and syncs the QFileSystemWatcher.
  void rebuildWatches();

  //! Compares a tracked file with its last known state, flags a change and re-adds its watch if needed.
  void checkFile( const QString &path );

  void onFileChanged( const QString &path );

  void onDirectoryChanged( const QString &path );

  std::unique_ptr<QFileSystemWatcher> watcher_;

  //! Paths as passed to addWatch. A path appears once per addWatch call (reference counting).
  std::vector<std::string> watched_paths_;

  //! Resolved target for each distinct path passed to addWatch.
  std::map<std::string, Target, std::less<>> targets_;

  //! Every directory below a watched directory (recursive) with its last seen entry listing.
  std::map<QString, QStringList> content_directories_;

  //! Files that were watched explicitly (not as part of a directory watch).
  std::set<QString> explicit_files_;

  //! Last known state of every tracked file (explicit files and files in content directories).
  std::map<QString, FileState> file_states_;

  mutable std::atomic<bool> changed_{ false };
};
} // namespace rqml

#endif // RQML_FILE_WATCHER_H
