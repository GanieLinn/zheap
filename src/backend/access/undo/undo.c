/*-------------------------------------------------------------------------
 *
 * undo.c
 *	  common undo code
 *
 * The undo subsystem consists of several logically separate subsystems
 * that work together to achieve a common goal. The code in this file
 * provides a limited amount of common infrastructure that can be used
 * by all of those various subsystems, and helps coordinate activities
 * such as startup and shutdown across subsystems.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/undo/undo.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "access/undo.h"
#include "access/undolog.h"
#include "access/undorecordset.h"
#include "access/xactundo.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/shmem.h"

/* Information needed while reading and writing checkpoint files. */
struct UndoCheckpointContext
{
	char	path[MAXPGPATH];
	int		fd;
	pg_crc32c crc;
};

static void AtProcExit_Undo(int code, Datum arg);
static void CleanUpUndoCheckPointFiles(XLogRecPtr checkPointRedo);
static void ReadRawUndoCheckpointData(UndoCheckpointContext *ctx,
									  void *buffer, Size nbytes);
static void WriteRawUndoCheckpointData(UndoCheckpointContext *ctx,
									   void *buffer, Size nbytes);

/*
 * UndoContext is a child of TopMemoryContext which is never reset. The only
 * reason for having a separate context is to make it easier to spot leaks or
 * excessive memory utilization.
 */
MemoryContext UndoContext = NULL;

/*
 * Figure out how much shared memory will be needed for undo.
 *
 * Each subsystem separately computes the space it requires, and we carefully
 * add up those values here.
 */
Size
UndoShmemSize(void)
{
	Size	size;

	size = UndoLogShmemSize();
	size = add_size(size, XactUndoShmemSize());

	return size;
}

/*
 * Initialize undo-related shared memory.
 *
 * Also, perform other initialization steps that need to be done very early.
 */
void
UndoShmemInit(void)
{
	/* First, make sure we can properly clean up on process exit. */
	on_shmem_exit(AtProcExit_Undo, 0);

	/* Initialize memory context. */
	Assert(UndoContext == NULL);
	UndoContext = AllocSetContextCreate(TopMemoryContext, "Undo",
										ALLOCSET_DEFAULT_SIZES);

	/* Now give various undo subsystems a chance to initialize. */
	UndoLogShmemInit();
	XactUndoShmemInit();
}

/*
 * Startup process work for the undo subsystem.
 *
 * Read the file generated by the last call to CheckPointUndo() and use that
 * to reinitialize shared memory state.
 */
void
StartupUndo(XLogRecPtr checkPointRedo)
{
	UndoCheckpointContext ctx;
	pg_crc32c	old_crc;

	/* If initdb is calling, there is no file to read yet. */
	if (IsBootstrapProcessingMode())
		return;

	/* Open the pg_undo file corresponding to the given checkpoint. */
	snprintf(ctx.path, MAXPGPATH, "pg_undo/%016" INT64_MODIFIER "X",
			 checkPointRedo);
	ctx.fd = OpenTransientFile(ctx.path, O_RDONLY | PG_BINARY);
	if (ctx.fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", ctx.path)));

	/* Initialize checksum calculation. */
	INIT_CRC32C(ctx.crc);

	/*
	 * Let each undo subsystem read its own data. The order of these calls
	 * needs to match CheckPointUndo().
	 */
	StartupUndoLogs(&ctx);
	StartupXactUndo(&ctx);

	/* Read checksum, close file, verify checksum. */
	ReadRawUndoCheckpointData(&ctx, &old_crc, sizeof(old_crc));
	CloseTransientFile(ctx.fd);
	FIN_CRC32C(ctx.crc);
	if (!EQ_CRC32C(ctx.crc, old_crc))
		ereport(ERROR,
				(errmsg("undo checkpoint file \"%s\" contains incorrect checksum",
						ctx.path)));
}

/*
 * Checkpoint time work for the undo subsystem.
 *
 * Write out a state file with sufficient information to reinitialize
 * critical shared memory state in the event that replay begins from this
 * checkpoint.
 */
void
CheckPointUndo(XLogRecPtr checkPointRedo, XLogRecPtr priorCheckPointRedo)
{
	UndoCheckpointContext ctx;

	/* Open the pg_undo file for the new checkpoint. */
	snprintf(ctx.path, MAXPGPATH, "pg_undo/%016" INT64_MODIFIER "X",
			 checkPointRedo);
	ctx.fd = OpenTransientFile(ctx.path, O_RDWR | O_CREAT | PG_BINARY);
	if (ctx.fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", ctx.path)));

	/* Initialize checksum calculation. */
	INIT_CRC32C(ctx.crc);

	/*
	 * Let each undo subsystem write some data. The order of these calls needs
	 * to match StartupUndo.
	 */
	CheckPointUndoLogs(&ctx);
	CheckPointXactUndo(&ctx);

	/* Write checksum. */
	FIN_CRC32C(ctx.crc);
	WriteRawUndoCheckpointData(&ctx, &ctx.crc, sizeof(ctx.crc));

	/* Call fsync() for both the file and the containing directory. */
	pgstat_report_wait_start(WAIT_EVENT_UNDO_CHECKPOINT_SYNC);
	pg_fsync(ctx.fd);
	if (CloseTransientFile(ctx.fd) < 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", ctx.path)));
	fsync_fname("pg_undo", true);
	pgstat_report_wait_end();

	/* Also clean up files we no longer need from previous checkpoints. */
	CleanUpUndoCheckPointFiles(priorCheckPointRedo);
}

/*
 * Read from open undo checkpoint file and update CRC calculation.
 */
void
ReadUndoCheckpointData(UndoCheckpointContext *ctx, void *buffer, Size nbytes)
{
	ReadRawUndoCheckpointData(ctx, buffer, nbytes);
	COMP_CRC32C(ctx->crc, buffer, nbytes);
}

/*
 * Write to open undo checkpoint file and update CRC calculation.
 */
void
WriteUndoCheckpointData(UndoCheckpointContext *ctx, void *buffer, Size nbytes)
{
	WriteRawUndoCheckpointData(ctx, buffer, nbytes);
	COMP_CRC32C(ctx->crc, buffer, nbytes);
}

/*
 * Shut down undo subsystems in the correct order.
 *
 * Generally, higher-level stuff should be shut down first.
 */
static void
AtProcExit_Undo(int code, Datum arg)
{
	AtProcExit_XactUndo();
	AtProcExit_UndoRecordSet();
	AtProcExit_UndoLog();
}

/*
 * Delete unreachable files under pg_undo.  Any files corresponding to LSN
 * positions before the previous checkpoint are no longer needed.
 */
static void
CleanUpUndoCheckPointFiles(XLogRecPtr checkPointRedo)
{
	DIR	   *dir;
	struct dirent *de;
	char	path[MAXPGPATH];
	char	oldest_path[MAXPGPATH];

	/*
	 * If a base backup is in progress, we can't delete any checkpoint
	 * snapshot files because one of them corresponds to the backup label but
	 * there could be any number of checkpoints during the backup.
	 */
	if (BackupInProgress())
		return;

	/* Otherwise keep only those >= the previous checkpoint's redo point. */
	snprintf(oldest_path, MAXPGPATH, "%016" INT64_MODIFIER "X",
			 checkPointRedo);
	dir = AllocateDir("pg_undo");
	while ((de = ReadDir(dir, "pg_undo")) != NULL)
	{
		/*
		 * Assume that fixed width uppercase hex strings sort the same way as
		 * the values they represent, so we can use strcmp to identify undo
		 * log snapshot files corresponding to checkpoints that we don't need
		 * anymore.  This assumption holds for ASCII.
		 */
		if (!(strlen(de->d_name) == UNDO_CHECKPOINT_FILENAME_LENGTH))
			continue;

		if (UndoCheckPointFilenamePrecedes(de->d_name, oldest_path))
		{
			snprintf(path, MAXPGPATH, "pg_undo/%s", de->d_name);
			if (unlink(path) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not unlink file \"%s\": %m", path)));
			elog(DEBUG2, "unlinking unreachable pg_undo file \"%s\"", path);
		}
	}
	FreeDir(dir);
}

/*
 * Read from the already-open undo checkpoint file.
 *
 * Report an error if we can't read the requested amount of data.
 */
static void
ReadRawUndoCheckpointData(UndoCheckpointContext *ctx, void *buffer,
						  Size nbytes)
{
	int		rc;

	pgstat_report_wait_start(WAIT_EVENT_UNDO_CHECKPOINT_READ);
	rc = read(ctx->fd, buffer, nbytes);
	pgstat_report_wait_end();

	if (rc < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", ctx->path)));

	if (rc < nbytes)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("could not read file \"%s\": read %d of %zu",
						ctx->path, rc, nbytes)));
}

/*
 * Write to the already-open undo checkpoint file.
 *
 * Report an error if we can't write the requested amount of data.
 */
static void
WriteRawUndoCheckpointData(UndoCheckpointContext *ctx, void *buffer,
						   Size nbytes)
{
	int		wc;

	pgstat_report_wait_start(WAIT_EVENT_UNDO_CHECKPOINT_WRITE);
	wc = write(ctx->fd, buffer, nbytes);
	pgstat_report_wait_end();

	if (wc < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", ctx->path)));

	if (wc < nbytes)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("could not write file \"%s\": wrote %d of %zu",
						ctx->path, wc, nbytes)));
}
