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

#include "file_system_watcher.hpp"

#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QtCore>

#include <algorithm>
#include <system_error>

namespace rqml
{
namespace
{

//! Ignore qmlc file changes because we create them. Some of them are temps and end in .qmlc.[RANDOMSTRING]
bool isIgnoredName( const QString &name )
{
  return name.contains( QLatin1String( ".qmlc." ) ) || name.endsWith( QLatin1String( ".qmlc" ) );
}

//! Sorted directory entries, without ignored files.
QStringList listDirectory( const QString &dir )
{
  QStringList entries = QDir( dir ).entryList(
      QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name );
  entries.erase( std::remove_if( entries.begin(), entries.end(), isIgnoredName ), entries.end() );
  return entries;
}

//! Collects dir and all of its subdirectories (following symlinks) together with their listings.
void collectDirectories( const QString &dir, std::map<QString, QStringList> &out )
{
  // Already visited, e.g. through a symlink or because watched directories overlap.
  // Checking before recursing also terminates symlink loops.
  if ( out.count( dir ) != 0 || !QFileInfo( dir ).isDir() )
    return;
  const QStringList entries = listDirectory( dir );
  out.emplace( dir, entries );
  const QDir qdir( dir );
  for ( const QString &name : entries ) {
    const QFileInfo info( qdir.filePath( name ) );
    if ( !info.isDir() )
      continue;
    const QString canonical = info.canonicalFilePath();
    if ( !canonical.isEmpty() )
      collectDirectories( canonical, out );
  }
}

QSet<QString> toSet( const QStringList &list ) { return QSet<QString>( list.begin(), list.end() ); }
} // namespace

FileSystemWatcher::FileSystemWatcher() : watcher_( std::make_unique<QFileSystemWatcher>() )
{
  // Using the watcher as context object disconnects these automatically when it is destroyed.
  QObject::connect( watcher_.get(), &QFileSystemWatcher::fileChanged, watcher_.get(),
                    [this]( const QString &path ) { onFileChanged( path ); } );
  QObject::connect( watcher_.get(), &QFileSystemWatcher::directoryChanged, watcher_.get(),
                    [this]( const QString &path ) { onDirectoryChanged( path ); } );
}

FileSystemWatcher::~FileSystemWatcher()
{
  // Destroy the watcher before the state its callbacks use.
  watcher_.reset();
}

bool FileSystemWatcher::isValid() const { return watcher_ != nullptr; }

bool FileSystemWatcher::addWatch( const std::string &path )
{
  if ( path.empty() )
    return false;
  if ( std::find( watched_paths_.begin(), watched_paths_.end(), path ) != watched_paths_.end() ) {
    // Already watching this path, just add another reference
    watched_paths_.push_back( path );
    return true;
  }

  // canonicalFilePath follows symlinks (including symlinked parent directories) and returns an
  // empty string if the path does not exist.
  const QString resolved = QFileInfo( QString::fromStdString( path ) ).canonicalFilePath();
  if ( resolved.isEmpty() )
    return false;
  const QFileInfo info( resolved );
  const bool is_directory = info.isDir();
  if ( !is_directory && !info.isFile() )
    return false;

  targets_.emplace( path, Target{ resolved, is_directory } );
  rebuildWatches();

  // A file is watched through its parent directory (to catch it being replaced or recreated),
  // so that directory watch has to be in place for the watch to work.
  const QString required = is_directory ? resolved : info.absolutePath();
  if ( !watcher_->directories().contains( required ) ) {
    qWarning( "Could not add file system watch for '%s'!", path.c_str() );
    targets_.erase( path );
    rebuildWatches();
    return false;
  }
  watched_paths_.push_back( path );
  return true;
}

void FileSystemWatcher::removeWatch( const std::string &path )
{
  auto it = std::find( watched_paths_.begin(), watched_paths_.end(), path );
  if ( it == watched_paths_.end() )
    return;
  watched_paths_.erase( it );
  // Only unregister if no other watches are left
  if ( std::find( watched_paths_.begin(), watched_paths_.end(), path ) != watched_paths_.end() )
    return;
  targets_.erase( path );
  rebuildWatches();
}

void FileSystemWatcher::removeAllWatches()
{
  watched_paths_.clear();
  targets_.clear();
  content_directories_.clear();
  explicit_files_.clear();
  file_states_.clear();
  const QStringList paths = watcher_->files() + watcher_->directories();
  if ( !paths.isEmpty() )
    watcher_->removePaths( paths );
  // Like the inotify implementation, changes on watches that no longer exist are dropped.
  changed_ = false;
}

bool FileSystemWatcher::checkForChanges() const { return changed_.exchange( false ); }

FileSystemWatcher::FileState FileSystemWatcher::readFileState( const QString &path )
{
  namespace fs = std::filesystem;
  FileState state;
  std::error_code error;
  const fs::path p( path.toStdU16String() );
  if ( !fs::is_regular_file( p, error ) )
    return state;
  state.exists = true;
  state.size = fs::file_size( p, error );
  if ( error )
    state.size = 0;
  state.modified = fs::last_write_time( p, error );
  if ( error )
    state.modified = {};
  return state;
}

void FileSystemWatcher::rebuildWatches()
{
  std::map<QString, QStringList> directories;
  std::set<QString> explicit_files;
  for ( const auto &[_, target] : targets_ ) {
    if ( target.is_directory )
      collectDirectories( target.path, directories );
    else
      explicit_files.insert( target.path );
  }

  // Files whose content is checked: explicitly watched files and all files in watched directories.
  std::set<QString> files = explicit_files;
  for ( const auto &[dir, entries] : directories ) {
    const QDir qdir( dir );
    for ( const QString &name : entries ) {
      const QString file = qdir.filePath( name );
      if ( QFileInfo( file ).isFile() )
        files.insert( file );
    }
  }

  // Keep the last known state of files that were already tracked, so changes between the last
  // event and this rebuild are not lost. New files start from their current state.
  std::map<QString, FileState> states;
  for ( const QString &file : files ) {
    auto old = file_states_.find( file );
    states.emplace( file, old != file_states_.end() ? old->second : readFileState( file ) );
  }

  QSet<QString> desired;
  for ( const auto &[dir, _] : directories ) desired.insert( dir );
  for ( const QString &file : explicit_files ) desired.insert( QFileInfo( file ).absolutePath() );
  for ( const auto &[file, state] : states ) {
    if ( state.exists )
      desired.insert( file );
  }

  const QSet<QString> current = toSet( watcher_->files() ) + toSet( watcher_->directories() );
  QStringList to_remove, to_add;
  for ( const QString &path : current ) {
    if ( !desired.contains( path ) )
      to_remove << path;
  }
  for ( const QString &path : desired ) {
    if ( !current.contains( path ) )
      to_add << path;
  }
  if ( !to_remove.isEmpty() )
    watcher_->removePaths( to_remove );
  if ( !to_add.isEmpty() ) {
    const QStringList failed = watcher_->addPaths( to_add );
    for ( const QString &path : failed )
      qWarning( "Could not add file system watch for '%s'!", qUtf8Printable( path ) );
  }

  content_directories_ = std::move( directories );
  explicit_files_ = std::move( explicit_files );
  file_states_ = std::move( states );
}

void FileSystemWatcher::checkFile( const QString &path )
{
  auto it = file_states_.find( path );
  if ( it == file_states_.end() )
    return;
  const FileState state = readFileState( path );
  // A file that disappears is not a change by itself (same as the inotify implementation); it counts
  // once it is recreated or replaced with new content.
  if ( state.exists && state != it->second )
    changed_ = true;
  it->second = state;
  // Many editors save by writing a temporary file and renaming it over the original. That replaces
  // the watched file, which removes it from the watcher on some platforms, so re-add it.
  if ( state.exists && !watcher_->files().contains( path ) )
    watcher_->addPath( path );
}

void FileSystemWatcher::onFileChanged( const QString &path )
{
  if ( isIgnoredName( QFileInfo( path ).fileName() ) )
    return;
  checkFile( path );
}

void FileSystemWatcher::onDirectoryChanged( const QString &path )
{
  bool needs_rebuild = false;
  if ( auto it = content_directories_.find( path ); it != content_directories_.end() ) {
    // Entries were created, deleted or renamed in a watched directory. Listings exclude qmlc files,
    // so the cache files we write ourselves do not count as a change.
    if ( !QFileInfo( path ).isDir() || listDirectory( path ) != it->second ) {
      changed_ = true;
      // Pick up new or removed files and subdirectories
      needs_rebuild = true;
    }
  }

  // Explicitly watched files in this directory may have been replaced or recreated.
  for ( const QString &file : explicit_files_ ) {
    if ( QFileInfo( file ).absolutePath() == path )
      checkFile( file );
  }

  if ( needs_rebuild )
    rebuildWatches();
}
} // namespace rqml
