// filesys.cc 
//	Routines to manage the overall operation of the file system.
//	Implements routines to map from textual file names to files.
//
//	Each file in the file system has:
//	   A file header, stored in a sector on disk 
//		(the size of the file header data structure is arranged
//		to be precisely the size of 1 disk sector)
//	   A number of data blocks
//	   An entry in the file system directory
//
// 	The file system consists of several data structures:
//	   A bitmap of free disk sectors (cf. bitmap.h)
//	   A directory of file names and file headers
//
//      Both the bitmap and the directory are represented as normal
//	files.  Their file headers are located in specific sectors
//	(sector 0 and sector 1), so that the file system can find them 
//	on bootup.
//
//	The file system assumes that the bitmap and directory files are
//	kept "open" continuously while Nachos is running.
//
//	For those operations (such as Create, Remove) that modify the
//	directory and/or bitmap, if the operation succeeds, the changes
//	are written immediately back to disk (the two files are kept
//	open during all this time).  If the operation fails, and we have
//	modified part of the directory and/or bitmap, we simply discard
//	the changed version, without writing it back to disk.
//
// 	Our implementation at this point has the following restrictions:
//
//	   there is no synchronization for concurrent accesses
//	   files have a fixed size, set when the file is created
//	   files cannot be bigger than about 3KB in size
//	   there is no hierarchical directory structure, and only a limited
//	     number of files can be added to the system
//	   there is no attempt to make the system robust to failures
//	    (if Nachos exits in the middle of an operation that modifies
//	    the file system, it may corrupt the disk)
//
// Copyright (c) 1992-1993 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation 
// of liability and disclaimer of warranty provisions.
#ifndef FILESYS_STUB

#include "copyright.h"
#include "debug.h"
#include "disk.h"
#include "pbitmap.h"
#include "directory.h"
#include "filehdr.h"
#include "filesys.h"
#include <vector>

// Sectors containing the file headers for the bitmap of free sectors,
// and the directory of files.  These file headers are placed in well-known 
// sectors, so that they can be located on boot-up.
#define FreeMapSector 		0
#define DirectorySector 	1

// Initial file sizes for the bitmap and directory; until the file system
// supports extensible files, the directory size sets the maximum number 
// of files that can be loaded onto the disk.
#define FreeMapFileSize 	(NumSectors / BitsInByte)
#define NumDirEntries 		10
#define DirectoryFileSize 	(sizeof(DirectoryEntry) * NumDirEntries)

//----------------------------------------------------------------------
// FileSystem::FileSystem
// 	Initialize the file system.  If format = TRUE, the disk has
//	nothing on it, and we need to initialize the disk to contain
//	an empty directory, and a bitmap of free sectors (with almost but
//	not all of the sectors marked as free).  
//
//	If format = FALSE, we just have to open the files
//	representing the bitmap and the directory.
//
//	"format" -- should we initialize the disk?
//----------------------------------------------------------------------

FileSystem::FileSystem(bool format)
{ 
	DEBUG(dbgFile, "Initializing the file system.");
	if (format) {
		PersistentBitmap *freeMap = new PersistentBitmap(NumSectors);
		Directory *directory = new Directory(NumDirEntries);
		FileHeader *mapHdr = new FileHeader;
		FileHeader *dirHdr = new FileHeader;
		int i = 0;
		DEBUG(dbgFile, "Formatting the file system.");

		// First, allocate space for FileHeaders for the directory and bitmap
		// (make sure no one else grabs these!)
		freeMap->Mark(FreeMapSector);	    
		freeMap->Mark(DirectorySector);

		// Second, allocate space for the data blocks containing the contents
		// of the directory and bitmap files.  There better be enough space!

		ASSERT(mapHdr->Allocate(freeMap, FreeMapFileSize));
		ASSERT(dirHdr->Allocate(freeMap, DirectoryFileSize));

		// Flush the bitmap and directory FileHeaders back to disk
		// We need to do this before we can "Open" the file, since open
		// reads the file header off of disk (and currently the disk has garbage
		// on it!).

		DEBUG(dbgFile, "Writing headers back to disk.");
		mapHdr->WriteBack(FreeMapSector);    
		dirHdr->WriteBack(DirectorySector);

		// OK to open the bitmap and directory files now
		// The file system operations assume these two files are left open
		// while Nachos is running.

		freeMapFile = new OpenFile(FreeMapSector);
		directoryFile = new OpenFile(DirectorySector);

		// Once we have the files "open", we can write the initial version
		// of each file back to disk.  The directory at this point is completely
		// empty; but the bitmap has been changed to reflect the fact that
		// sectors on the disk have been allocated for the file headers and
		// to hold the file data for the directory and bitmap.

		DEBUG(dbgFile, "Writing bitmap and directory back to disk.");
		freeMap->WriteBack(freeMapFile);	 // flush change

		directory->WriteBack(directoryFile);

		if (debug->IsEnabled('f')) {
			freeMap->Print();
			directory->Print();
		}
		/*
		   for(i=0;i<SYS_MAX_OPEN_FILE_NUM;i++){
		   sysOpenFileTable[i] = NULL;
		   }*/
		delete freeMap; 
		delete directory; 
		delete mapHdr; 
		delete dirHdr;
	} else {
		// if we are not formatting the disk, just open the files representing
		// the bitmap and directory; these are left open while Nachos is running
		freeMapFile = new OpenFile(FreeMapSector);
		directoryFile = new OpenFile(DirectorySector);
	}
}

//----------------------------------------------------------------------
// MP4 mod tag
// FileSystem::~FileSystem
//----------------------------------------------------------------------
FileSystem::~FileSystem()
{
	delete freeMapFile;
	delete directoryFile;
}

//----------------------------------------------------------------------
// FileSystem::Create
// 	Create a file in the Nachos file system (similar to UNIX create).
//	Since we can't increase the size of files dynamically, we have
//	to give Create the initial size of the file.
//
//	The steps to create a file are:
//	  Make sure the file doesn't already exist
//        Allocate a sector for the file header
// 	  Allocate space on disk for the data blocks for the file
//	  Add the name to the directory
//	  Store the new file header on disk 
//	  Flush the changes to the bitmap and the directory back to disk
//
//	Return TRUE if everything goes ok, otherwise, return FALSE.
//
// 	Create fails if:
//   		file is already in directory
//	 	no free space for file header
//	 	no free entry for file in directory
//	 	no free space for data blocks for the file 
//
// 	Note that this implementation assumes there is no concurrent access
//	to the file system!
//
//	"name" -- name of file to be created
//	"initialSize" -- size of file to be created
//----------------------------------------------------------------------

	bool
FileSystem::Create(char *name, int initialSize)
{
	Directory *directory;
	PersistentBitmap *freeMap;
	FileHeader *hdr;
	int sector;
	bool success;

	DEBUG(dbgFile, "Creating file " << name << " size " << initialSize);

	OpenFile * dirFile = GoDirectory(&name);

	directory = new Directory(NumDirEntries);
	directory->FetchFrom(dirFile);

	if (directory->Find(name) != -1)
		success = FALSE;			// file is already in directory
	else {	
		freeMap = new PersistentBitmap(freeMapFile,NumSectors);
		sector = freeMap->FindAndSet();	// find a sector to hold the file header
		if (sector == -1) 		
			success = FALSE;		// no free block for file header 
		else if (!directory->Add(name, sector))
			success = FALSE;	// no space in directory
		else {
			hdr = new FileHeader;
			if (!hdr->Allocate(freeMap, initialSize))
				success = FALSE;	// no space on disk for data
			else {	
				success = TRUE;
				// everthing worked, flush all changes back to disk
				hdr->WriteBack(sector);
				directory->WriteBack(dirFile);
				if(IsDir(name)){	//format subdir
					OpenFile * newDirFile = new OpenFile(sector);
					Directory * newDir = new Directory(NumDirEntries);

					newDir->WriteBack(newDirFile); // write back a empty table to the dir file = format
					delete newDir;
					delete newDirFile;
				}
				if(dirFile != directoryFile) delete dirFile; //root dir file should keep opening

				freeMap->WriteBack(freeMapFile);
			}
			delete hdr;
		}
		delete freeMap;
	}
	delete [] name;
	delete directory;
	return success;
}

//----------------------------------------------------------------------
// FileSystem::Open
// 	Open a file for reading and writing.  
//	To open a file:
//	  Find the location of the file's header, using the directory 
//	  Bring the header into memory
//
//	"name" -- the text name of the file to be opened
//----------------------------------------------------------------------

	OpenFile *
FileSystem::Open(char *name)
{ 
	Directory *directory = new Directory(NumDirEntries);
	OpenFile *openFile = NULL;
	int sector;
	int fd = -1;
	DEBUG(dbgFile, "Opening file" << name);


	OpenFile* dirFile = GoDirectory(&name);
	directory->FetchFrom(dirFile);
	if(dirFile != directoryFile) delete dirFile; //root dir file should keep opening

	if(name==NULL || IsDir(name)) {
		std::cout<<"FileSystem::Open : Bad open path."<<std::endl;
		if(name!=NULL) delete [] name;
		return NULL;
	}
	sector = directory->Find(name); 
	// TODO: allocate a new entry in system-wide table[done]
	if (sector >= 0){		

		openFile = new OpenFile(sector);	// name was found in directory 
		if(GetSysFd(&fd)){
			openFile->SetFd(fd);
			DEBUG(dbgMp4, "Open file in FileSystem::Open,"<< "name="<<name<<",fd="<<fd);
		}
		else{
			delete openFile;
			openFile = NULL;
		}
	}
	delete [] name;
	delete directory;
	return openFile;				// return NULL if not found
}

//----------------------------------------------------------------------
// FileSystem::Remove
// 	Delete a file from the file system.  This requires:
//	    Remove it from the directory
//	    Delete the space for its header
//	    Delete the space for its data blocks
//	    Write changes to directory, bitmap back to disk
//
//	Return TRUE if the file was deleted, FALSE if the file wasn't
//	in the file system.
//
//	"name" -- the text name of the file to be removed
//----------------------------------------------------------------------
bool
FileSystem::Remove(char *name){
	return Remove(name,false);
}



	bool
FileSystem::Remove(char *name,bool recurRemoveFlag)
{ 
	Directory *directory;
	PersistentBitmap *freeMap;
	FileHeader *fileHdr;
	int sector;
	bool success = TRUE;

	directory = new Directory(NumDirEntries);
	OpenFile* dirFile = GoDirectory(&name);
	directory->FetchFrom(dirFile);

	sector = directory->Find(name);
	if (sector == -1) {
		delete directory;
		return FALSE;			 // file not found 
	}

	freeMap = new PersistentBitmap(freeMapFile,NumSectors);

	if(recurRemoveFlag && IsDir(name)){
		Directory *subDir = new Directory(NumDirEntries);
		OpenFile * subDirFile = new OpenFile(sector);
		subDir->FetchFrom(subDirFile);

		success &= subDir->RecRemove(freeMap);
        subDir->WriteBack(subDirFile);  // not nessery
		delete subDirFile;
		delete subDir;
	}

	fileHdr = new FileHeader;
	fileHdr->FetchFrom(sector);


	fileHdr->Deallocate(freeMap);  		// remove data blocks
	freeMap->Clear(sector);			// remove header block
	directory->Remove(name);

	freeMap->WriteBack(freeMapFile);		// flush to disk
	directory->WriteBack(dirFile);        // flush to disk

	if(dirFile != directoryFile) delete dirFile; //root dir file should keep opening
	delete [] name;
	delete fileHdr;
	delete directory;
	delete freeMap;

	return success;
} 

//----------------------------------------------------------------------
// FileSystem::List
// 	List all the files in the file system directory.
//----------------------------------------------------------------------

	void
FileSystem::List()
{
	Directory *directory = new Directory(NumDirEntries);
	directory->FetchFrom(directoryFile);
	directory->List();
	delete directory;
}

	void
FileSystem::List(char* path,bool recursiveListFlag)
{
	Directory *directory = new Directory(NumDirEntries);
	OpenFile * dirFile = GoDirectory(&path);
	directory->FetchFrom(dirFile);
	if(recursiveListFlag) directory->List(0);
	else {
		if(dirFile==directoryFile && !IsDir(path)){
			directory->List();
			return;
		}

		// open the sub-dir and list it
		char * name = path;
		Directory * subDir = new Directory(NumDirEntries);
		OpenFile * subDirFile = new OpenFile(directory->Find(name));
		subDir->FetchFrom(subDirFile);

		subDir->List();
		delete subDir;
		delete subDirFile;
	}
	if(dirFile!=directoryFile) delete dirFile;
	delete directory;
}
//----------------------------------------------------------------------
// FileSystem::Print
// 	Print everything about the file system:
//	  the contents of the bitmap
//	  the contents of the directory
//	  for each file in the directory,
//	      the contents of the file header
//	      the data in the file
//----------------------------------------------------------------------

	void
FileSystem::Print()
{
	FileHeader *bitHdr = new FileHeader;
	FileHeader *dirHdr = new FileHeader;
	PersistentBitmap *freeMap = new PersistentBitmap(freeMapFile,NumSectors);
	Directory *directory = new Directory(NumDirEntries);

	printf("Bit map file header:\n");
	bitHdr->FetchFrom(FreeMapSector);
	bitHdr->Print();

	printf("Directory file header:\n");
	dirHdr->FetchFrom(DirectorySector);
	dirHdr->Print();

	freeMap->Print();

	directory->FetchFrom(directoryFile);
	directory->Print();

	delete bitHdr;
	delete dirHdr;
	delete freeMap;
	delete directory;
} 

int 
FileSystem::Read(char *buf, int size, int fd){
	OpenFile *opFile = GetOpenFileTable(fd);

	if(opFile){
		return opFile->Read(buf,size);
	}
	return -1;
}

int 
FileSystem::Write(char *buf, int size, int fd){

	OpenFile *opFile = GetOpenFileTable(fd);

	if(opFile){
		return opFile->Write(buf,size);
	}

	return -1;
}

int 
FileSystem::Seek(int position,int fd){
	return 1;
}

int 
FileSystem::Close(int fd){

	OpenFile *opFile = GetOpenFileTable(fd);

	//SetOpenFileTable(fd,NULL);
	sysOpFileTable.erase(fd);
	delete opFile;
	return 1;
}

bool FileSystem::GetSysFd(int *fdout){

	int i = 0;
	int fd = fdPosition;
	OpenFile *opFile = NULL;
	while(i<SYS_MAX_OPEN_FILE_NUM){
		fd = (fd+i)%SYS_MAX_OPEN_FILE_NUM;
		opFile = sysOpFileTable[fd];
		if(opFile==NULL){
			*fdout = fd;
			fdPosition = (fd+1);// assume next one is free
			return TRUE;
		}
		i++;
	}

	return FALSE;
}

void FileSystem::SetOpenFileTable(int fd, OpenFile *openFile){
	sysOpFileTable[fd] = openFile;
}

OpenFile* FileSystem::GetOpenFileTable(int fd){

	return sysOpFileTable[fd];
}

std::vector<char*>& FileSystem::PreprocessPath(char* path, std::vector<char*>& pathQueue){
	const int NAME_SIZE = 255;
	char *name = NULL;


	for(int i=0, pre_i = 0;i<strlen(path);i++){
		if(i==pre_i) 
			name = new char[NAME_SIZE];		//NOTICE: you should delete this yourself after used.


		name[i-pre_i] = path[i];
		if(path[i]=='/'||i+1==strlen(path)) {
			name[i-pre_i+1] = '\0';
			pathQueue.push_back(name);
			pre_i = i+1;
		}
	}
}

bool FileSystem::IsDir(char* name){
	return (name[strlen(name)-1] == '/');
}

void FileSystem::CleanQueue(std::vector<char*>& queue){
	while(!queue.empty()){
		char * toBeDelete = queue.front();
		queue.erase(queue.begin());
		delete [] toBeDelete;
	}
}

OpenFile * FileSystem::GoDirectory(char** name){
	std::vector<char*> pathQueue;
	PreprocessPath(*name,pathQueue);
	//delete [] *name; //delete the input string (which is the absolute path)	
	OpenFile * dirFile = NULL;
	Directory* directory = new Directory(NumDirEntries);

	while(!pathQueue.empty()){
		*name = pathQueue.front();
		pathQueue.erase(pathQueue.begin()); 	//pop_front

		if(strcmp(*name,"/")==0){				//root dir
			directory->FetchFrom(directoryFile);
			dirFile = directoryFile;
		}else if(IsDir(*name)){		//sub dir
			int subDirSector = directory->Find(*name);
			if(subDirSector==-1) {
				ASSERT(pathQueue.empty());		//this dir is not exist,
				// and the path access it's child
				// bad request, terminate the system.
				break; //not exist, going to be create a dir.
			}
			if(pathQueue.empty()) break; // no create , just need to find the directory (ex: Remove)

			if(dirFile != directoryFile) delete dirFile;	// delete last dir file
			// BUT! if last dir is root , do nothing.
			dirFile = new OpenFile(subDirSector);
			directory->FetchFrom(dirFile);
		}else{	//this is a file
			ASSERT(pathQueue.empty());	// path should be the last file
			break; // going to create a file
		}
		delete [] *name;
	}
	delete directory;

	return dirFile;
}

#endif // FILESYS_STUB
