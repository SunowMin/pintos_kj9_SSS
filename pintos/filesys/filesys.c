#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "devices/disk.h"

/* The disk that contains the file system. */
struct disk *filesys_disk;

static void do_format (void);

/* Initializes the file system module.
 * If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) {
	filesys_disk = disk_get (0, 1);
	if (filesys_disk == NULL)
		PANIC ("hd0:1 (hdb) not present, file system initialization failed");

	inode_init ();

#ifdef EFILESYS
	fat_init ();

	if (format)
		do_format ();

	fat_open ();
#else
	/* Original FS */
	free_map_init ();

	if (format)
		do_format ();

	free_map_open ();
#endif
}

/* Shuts down the file system module, writing any unwritten data
 * to disk. */
void
filesys_done (void) {
	/* Original FS */
#ifdef EFILESYS
	fat_close ();
#else
	free_map_close ();
#endif
}

/* Creates a file named NAME with the given INITIAL_SIZE.
 * Returns true if successful, false otherwise.
 * Fails if a file named NAME already exists,
 * or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) {
	// 새로 만들 파일의 inode를 저장할 디스크 섹터 번호를 저장할 변수
	// -> 운영체제가 파일을 디스크에 저장할 때는 파일 내용을 섹터 단위로 나눠서 저장
	// 초기값은 0 (즉, 아직 할당 안됨)
	disk_sector_t inode_sector = 0;
	// 루트 디렉토리를 엶. 실패시 dir=NULL;
	struct dir *dir = dir_open_root ();
	// 4가지 조건을 만족해야 파일 생성이 성공
	bool success = (
			dir != NULL
			// inode를 저장할 디스크 섹터 1개를 할당받았는가?
			&& free_map_allocate (1, &inode_sector)
			// 할당받은 섹터에 inode_disk 구조체를 생성하고, 파일길이만큼의 데이터 공간을 초기화
			&& inode_create (inode_sector, initial_size)
			// 디렉토리 엔트리에 파일이름과 inode_sector번호를 등록
			&& dir_add (dir, name, inode_sector));
	// 4단계 중 하나라도 실패했다면
	if (!success && inode_sector != 0)
		// 이미 할당해 둔 섹터를 반환해서 메모리(디스크) 누수 방지
		free_map_release (inode_sector, 1);
	// 열었던 디렉토리 핸들을 닫고 리소스 해제
	dir_close (dir);

	return success;
}

/* Opens the file with the given NAME.
 * Returns the new file if successful or a null pointer
 * otherwise.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name) {
	struct dir *dir = dir_open_root ();
	struct inode *inode = NULL;

	if (dir != NULL)
		dir_lookup (dir, name, &inode);
	dir_close (dir);

	return file_open (inode);
}

/* Deletes the file named NAME.
 * Returns true if successful, false on failure.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) {
	// 루트 디렉토리 열기 - 성공:디렉토리핸들(dir), 실패:NULL 반환
	struct dir *dir = dir_open_root ();
	// dir이 NULL이 아닌게 맞으면 dir_remove(dir, name)을 호출해 실제 삭제를 시도
	// dir_remove는 디렉토리 엔트리에서 name을 찾아 지우고 해당 파일의 inode를 삭제표시 또는 해제
	bool success = dir != NULL && dir_remove (dir, name);
	// 열었던 루트 디렉토리를 닫아 리소스를 해제
	dir_close (dir);

	// 삭제 작업의 성공 여부를 호출자에게 반환
	return success;
}

/* Formats the file system. */
static void
do_format (void) {
	printf ("Formatting file system...");

#ifdef EFILESYS
	/* Create FAT and save it to the disk. */
	fat_create ();
	fat_close ();
#else
	free_map_create ();
	if (!dir_create (ROOT_DIR_SECTOR, 16))
		PANIC ("root directory creation failed");
	free_map_close ();
#endif

	printf ("done.\n");
}
