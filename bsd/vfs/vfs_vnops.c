/*
 * Copyright (c) 2000-2008 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 * 
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/* Copyright (c) 1995 NeXT Computer, Inc. All Rights Reserved */
/*
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)vfs_vnops.c	8.14 (Berkeley) 6/15/95
 *
 */
/*
 * NOTICE: This file was modified by SPARTA, Inc. in 2005 to introduce
 * support for mandatory and extensible security protections.  This notice
 * is included in support of clause 2.2 (b) of the Apple Public License,
 * Version 2.0.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/file_internal.h>
#include <sys/stat.h>
#include <sys/proc_internal.h>
#include <sys/kauth.h>
#include <sys/mount_internal.h>
#include <sys/namei.h>
#include <sys/vnode_internal.h>
#include <sys/ioctl.h>
#include <sys/tty.h>
/* Temporary workaround for ubc.h until <rdar://4714366 is resolved */
#define ubc_setcred ubc_setcred_deprecated
#include <sys/ubc.h>
#undef ubc_setcred
int	ubc_setcred(struct vnode *, struct proc *);
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/fsevents.h>
#include <sys/kdebug.h>
#include <sys/xattr.h>
#include <sys/ubc_internal.h>
#include <sys/uio_internal.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>

#include <vm/vm_kern.h>
#include <vm/vm_map.h>

#include <miscfs/specfs/specdev.h>
#include <miscfs/fifofs/fifo.h>

#if CONFIG_MACF
#include <security/mac_framework.h>
#endif


static int vn_closefile(struct fileglob *fp, vfs_context_t ctx);
static int vn_ioctl(struct fileproc *fp, u_long com, caddr_t data,
			vfs_context_t ctx);
static int vn_read(struct fileproc *fp, struct uio *uio, int flags,
			vfs_context_t ctx);
static int vn_write(struct fileproc *fp, struct uio *uio, int flags,
			vfs_context_t ctx);
static int vn_select( struct fileproc *fp, int which, void * wql,
			vfs_context_t ctx);
static int vn_kqfilt_add(struct fileproc *fp, struct knote *kn,
			vfs_context_t ctx);
static void filt_vndetach(struct knote *kn);
static int filt_vnode(struct knote *kn, long hint);
#if 0
static int vn_kqfilt_remove(struct vnode *vp, uintptr_t ident,
			vfs_context_t ctx);
#endif

struct 	fileops vnops =
	{ vn_read, vn_write, vn_ioctl, vn_select, vn_closefile, vn_kqfilt_add, NULL };

struct  filterops vnode_filtops = { 
	.f_isfd = 1, 
	.f_attach = NULL, 
	.f_detach = filt_vndetach, 
	.f_event = filt_vnode
};

/*
 * Common code for vnode open operations.
 * Check permissions, and call the VNOP_OPEN or VNOP_CREATE routine.
 *
 * XXX the profusion of interfaces here is probably a bad thing.
 */
int
vn_open(struct nameidata *ndp, int fmode, int cmode)
{
	return(vn_open_modflags(ndp, &fmode, cmode));
}

int
vn_open_modflags(struct nameidata *ndp, int *fmodep, int cmode)
{
	struct vnode_attr va;

	VATTR_INIT(&va);
	VATTR_SET(&va, va_mode, cmode);
	
	return(vn_open_auth(ndp, fmodep, &va));
}

/*
 * Open a file with authorization, updating the contents of the structures
 * pointed to by ndp, fmodep, and vap as necessary to perform the requested
 * operation.  This function is used for both opens of existing files, and
 * creation of new files.
 *
 * Parameters:	ndp			The nami data pointer describing the
 *					file
 *		fmodep			A pointer to an int containg the mode
 *					information to be used for the open
 *		vap			A pointer to the vnode attribute
 *					descriptor to be used for the open
 *
 * Indirect:	*			Contents of the data structures pointed
 *					to by the parameters are modified as
 *					necessary to the requested operation.
 *
 * Returns:	0			Success
 *		!0			errno value
 *
 * Notes:	The kauth_filesec_t in 'vap', if any, is in host byte order.
 *
 *		The contents of '*ndp' will be modified, based on the other
 *		arguments to this function, and to return file and directory
 *		data necessary to satisfy the requested operation.
 *
 *		If the file does not exist and we are creating it, then the
 *		O_TRUNC flag will be cleared in '*fmodep' to indicate to the
 *		caller that the file was not truncated.
 *
 *		If the file exists and the O_EXCL flag was not specified, then
 *		the O_CREAT flag will be cleared in '*fmodep' to indicate to
 *		the caller that the existing file was merely opened rather
 *		than created.
 *
 *		The contents of '*vap' will be modified as necessary to
 *		complete the operation, including setting of supported
 *		attribute, clearing of fields containing unsupported attributes
 *		in the request, if the request proceeds without them, etc..
 *
 * XXX:		This function is too complicated in actings on its arguments
 *
 * XXX:		We should enummerate the possible errno values here, and where
 *		in the code they originated.
 */
int
vn_open_auth(struct nameidata *ndp, int *fmodep, struct vnode_attr *vap)
{
	struct vnode *vp;
	struct vnode *dvp;
	vfs_context_t ctx = ndp->ni_cnd.cn_context;
	int error;
	int fmode;
	uint32_t origcnflags;
	kauth_action_t action;

again:
	vp = NULL;
	dvp = NULL;
	fmode = *fmodep;
	origcnflags = ndp->ni_cnd.cn_flags;
	if (fmode & O_CREAT) {
	        if ( (fmode & O_DIRECTORY) ) {
		        error = EINVAL;
			goto out;
		}
		ndp->ni_cnd.cn_nameiop = CREATE;
		/* Inherit USEDVP, vnode_open() supported flags only */
		ndp->ni_cnd.cn_flags &= (USEDVP | NOCROSSMOUNT | DOWHITEOUT);
		ndp->ni_cnd.cn_flags |= LOCKPARENT | LOCKLEAF | AUDITVNPATH1;
#if NAMEDRSRCFORK
		/* open calls are allowed for resource forks. */
		ndp->ni_cnd.cn_flags |= CN_ALLOWRSRCFORK;
#endif
		if ((fmode & O_EXCL) == 0 && (fmode & O_NOFOLLOW) == 0 && (origcnflags & FOLLOW) != 0)
			ndp->ni_cnd.cn_flags |= FOLLOW;
		if ( (error = namei(ndp)) )
			goto out;
		dvp = ndp->ni_dvp;
		vp = ndp->ni_vp;

 		/* not found, create */
		if (vp == NULL) {
 			/* must have attributes for a new file */
 			if (vap == NULL) {
 				error = EINVAL;
				goto badcreate;
 			}

			VATTR_SET(vap, va_type, VREG);
#if CONFIG_MACF
			error = mac_vnode_check_create(ctx,
			    dvp, &ndp->ni_cnd, vap);
			if (error)
				goto badcreate;
#endif /* MAC */

			/* authorize before creating */
 			if ((error = vnode_authorize(dvp, NULL, KAUTH_VNODE_ADD_FILE, ctx)) != 0)
				goto badcreate;

			if (fmode & O_EXCL)
				vap->va_vaflags |= VA_EXCLUSIVE;
#if NAMEDRSRCFORK
			if (ndp->ni_cnd.cn_flags & CN_WANTSRSRCFORK) {
				if ((error = vnode_makenamedstream(dvp, &ndp->ni_vp, XATTR_RESOURCEFORK_NAME, 0, ctx)) != 0)
					goto badcreate;
			} else
#endif
			if ((error = vn_create(dvp, &ndp->ni_vp, &ndp->ni_cnd, vap, 0, ctx)) != 0)
				goto badcreate;
			
			vp = ndp->ni_vp;

			if (vp) {
				int	update_flags = 0;

			        // Make sure the name & parent pointers are hooked up
			        if (vp->v_name == NULL)
					update_flags |= VNODE_UPDATE_NAME;
				if (vp->v_parent == NULLVP)
				        update_flags |= VNODE_UPDATE_PARENT;

				if (update_flags)
				        vnode_update_identity(vp, dvp, ndp->ni_cnd.cn_nameptr, ndp->ni_cnd.cn_namelen, ndp->ni_cnd.cn_hash, update_flags);

#if CONFIG_FSE
				if (need_fsevent(FSE_CREATE_FILE, vp)) {
					vnode_put(dvp);
					dvp = NULL;
				        add_fsevent(FSE_CREATE_FILE, ctx,
						    FSE_ARG_VNODE, vp,
						    FSE_ARG_DONE);
				}
#endif

			}
			/*
			 * nameidone has to happen before we vnode_put(dvp)
			 * and clear the ni_dvp field, since it may need
			 * to release the fs_nodelock on the dvp
			 */
badcreate:
			nameidone(ndp);
			ndp->ni_dvp = NULL;

			if (dvp) {
				vnode_put(dvp);
			}

			if (error) {
				/*
				 * Check for a creation or unlink race.
				 */
				if (((error == EEXIST) && !(fmode & O_EXCL)) ||
					   ((error == ENOENT) && (fmode & O_CREAT))){
					goto again;
				}
				goto bad;
			}
			fmode &= ~O_TRUNC;
		} else {
			nameidone(ndp);
			ndp->ni_dvp = NULL;
			vnode_put(dvp);

			if (fmode & O_EXCL) {
				error = EEXIST;
				goto bad;
			}
			fmode &= ~O_CREAT;
		}
	} else {
		ndp->ni_cnd.cn_nameiop = LOOKUP;
		/* Inherit USEDVP, vnode_open() supported flags only */
		ndp->ni_cnd.cn_flags &= (USEDVP | NOCROSSMOUNT | DOWHITEOUT);
		ndp->ni_cnd.cn_flags |= FOLLOW | LOCKLEAF | AUDITVNPATH1;
#if NAMEDRSRCFORK
		/* open calls are allowed for resource forks. */
		ndp->ni_cnd.cn_flags |= CN_ALLOWRSRCFORK;
#endif
		/* preserve NOFOLLOW from vnode_open() */
		if (fmode & O_NOFOLLOW || fmode & O_SYMLINK || (origcnflags & FOLLOW) == 0) {
		    ndp->ni_cnd.cn_flags &= ~FOLLOW;
		}

		if ( (error = namei(ndp)) )
			goto out;
		vp = ndp->ni_vp;
		nameidone(ndp);
		ndp->ni_dvp = NULL;

		if ( (fmode & O_DIRECTORY) && vp->v_type != VDIR ) {
		        error = ENOTDIR;
			goto bad;
		}
	}

	if (vp->v_type == VSOCK && vp->v_tag != VT_FDESC) {
		error = EOPNOTSUPP;	/* Operation not supported on socket */
		goto bad;
	}

	if (vp->v_type == VLNK && (fmode & O_NOFOLLOW) != 0) {
		error = ELOOP;	/* O_NOFOLLOW was specified and the target is a symbolic link */
		goto bad;
	}

	/* authorize open of an existing file */
	if ((fmode & O_CREAT) == 0) {

		/* disallow write operations on directories */
		if (vnode_isdir(vp) && (fmode & (FWRITE | O_TRUNC))) {
			error = EISDIR;
			goto bad;
		}

#if CONFIG_MACF
		error = mac_vnode_check_open(ctx, vp, fmode);
		if (error)
			goto bad;
#endif

		/* compute action to be authorized */
		action = 0;
		if (fmode & FREAD) {
			action |= KAUTH_VNODE_READ_DATA;
		}
		if (fmode & (FWRITE | O_TRUNC)) {
			/*
			 * If we are writing, appending, and not truncating,
			 * indicate that we are appending so that if the
			 * UF_APPEND or SF_APPEND bits are set, we do not deny
			 * the open.
			 */
			if ((fmode & O_APPEND) && !(fmode & O_TRUNC)) {
				action |= KAUTH_VNODE_APPEND_DATA;
			} else {
			action |= KAUTH_VNODE_WRITE_DATA;
			}
		}
		if ((error = vnode_authorize(vp, NULL, action, ctx)) != 0)
			goto bad;
		

		//
		// if the vnode is tagged VOPENEVT and the current process
		// has the P_CHECKOPENEVT flag set, then we or in the O_EVTONLY
		// flag to the open mode so that this open won't count against
		// the vnode when carbon delete() does a vnode_isinuse() to see
		// if a file is currently in use.  this allows spotlight
		// importers to not interfere with carbon apps that depend on
		// the no-delete-if-busy semantics of carbon delete().
		//
		if ((vp->v_flag & VOPENEVT) && (current_proc()->p_flag & P_CHECKOPENEVT)) {
		    fmode |= O_EVTONLY;
		}

	}

	if ( (error = VNOP_OPEN(vp, fmode, ctx)) ) {
		goto bad;
	}
	if ( (error = vnode_ref_ext(vp, fmode)) ) {
		goto bad2;
	}

	/* call out to allow 3rd party notification of open. 
	 * Ignore result of kauth_authorize_fileop call.
	 */
	kauth_authorize_fileop(vfs_context_ucred(ctx), KAUTH_FILEOP_OPEN, 
						   (uintptr_t)vp, 0);

	*fmodep = fmode;
	return (0);
bad2:
	VNOP_CLOSE(vp, fmode, ctx);
bad:
	ndp->ni_vp = NULL;
	if (vp) {
#if NAMEDRSRCFORK
		/* Aggressively recycle shadow files if we error'd out during open() */
		if ((vnode_isnamedstream(vp)) &&
			(vp->v_parent != NULLVP) && 
			(vnode_isshadow(vp))) {
				vnode_recycle(vp);
		}
#endif
		vnode_put(vp);
		/*
		 * Check for a race against unlink.  We had a vnode
		 * but according to vnode_authorize or VNOP_OPEN it
		 * no longer exists.
		 *
		 * EREDRIVEOPEN: means that we were hit by the tty allocation race.
		 */
		if (((error == ENOENT) && (*fmodep & O_CREAT)) || (error == EREDRIVEOPEN)) {
			goto again;
		}
	}
out:
	return (error);
}

#if vn_access_DEPRECATED
/*
 * Authorize an action against a vnode.  This has been the canonical way to
 * ensure that the credential/process/etc. referenced by a vfs_context
 * is granted the rights called out in 'mode' against the vnode 'vp'.
 *
 * Unfortunately, the use of VREAD/VWRITE/VEXEC makes it very difficult
 * to add support for more rights.  As such, this interface will be deprecated
 * and callers will use vnode_authorize instead.
 */
int
vn_access(vnode_t vp, int mode, vfs_context_t context)
{
 	kauth_action_t	action;
  
  	action = 0;
 	if (mode & VREAD)
 		action |= KAUTH_VNODE_READ_DATA;
 	if (mode & VWRITE)
		action |= KAUTH_VNODE_WRITE_DATA;
  	if (mode & VEXEC)
  		action |= KAUTH_VNODE_EXECUTE;
  
 	return(vnode_authorize(vp, NULL, action, context));
}
#endif	/* vn_access_DEPRECATED */

/*
 * Vnode close call
 */
int
vn_close(struct vnode *vp, int flags, vfs_context_t ctx)
{
	int error;

#if CONFIG_FSE
	if (flags & FWASWRITTEN) {
	        if (need_fsevent(FSE_CONTENT_MODIFIED, vp)) {
		        add_fsevent(FSE_CONTENT_MODIFIED, ctx,
				    FSE_ARG_VNODE, vp,
				    FSE_ARG_DONE);
		}
	}
#endif

#if NAMEDRSRCFORK
	/* Sync data from resource fork shadow file if needed. */
	if ((vp->v_flag & VISNAMEDSTREAM) && 
	    (vp->v_parent != NULLVP) &&
	    vnode_isshadow(vp)) {
		if (flags & FWASWRITTEN) {
			(void) vnode_flushnamedstream(vp->v_parent, vp, ctx);
		}
	}
#endif
	
	/* work around for foxhound */
	if (vnode_isspec(vp))
		(void)vnode_rele_ext(vp, flags, 0);

	error = VNOP_CLOSE(vp, flags, ctx);

	if (!vnode_isspec(vp))
		(void)vnode_rele_ext(vp, flags, 0);
	
	return (error);
}

static int
vn_read_swapfile(
	struct vnode	*vp,
	uio_t		uio)
{
	int	error;
	off_t	swap_count, this_count;
	off_t	file_end, read_end;
	off_t	prev_resid;
	char 	*my_swap_page;

	/*
	 * Reading from a swap file will get you zeroes.
	 */

	my_swap_page = NULL;
	error = 0;
	swap_count = uio_resid(uio);

	file_end = ubc_getsize(vp);
	read_end = uio->uio_offset + uio_resid(uio);
	if (uio->uio_offset >= file_end) {
		/* uio starts after end of file: nothing to read */
		swap_count = 0;
	} else if (read_end > file_end) {
		/* uio extends beyond end of file: stop before that */
		swap_count -= (read_end - file_end);
	}

	while (swap_count > 0) {
		if (my_swap_page == NULL) {
			MALLOC(my_swap_page, char *, PAGE_SIZE,
			       M_TEMP, M_WAITOK);
			memset(my_swap_page, '\0', PAGE_SIZE);
			/* add an end-of-line to keep line counters happy */
			my_swap_page[PAGE_SIZE-1] = '\n';
		}
		this_count = swap_count;
		if (this_count > PAGE_SIZE) {
			this_count = PAGE_SIZE;
		}

		prev_resid = uio_resid(uio);
		error = uiomove((caddr_t) my_swap_page,
				this_count,
				uio);
		if (error) {
			break;
		}
		swap_count -= (prev_resid - uio_resid(uio));
	}
	if (my_swap_page != NULL) {
		FREE(my_swap_page, M_TEMP);
		my_swap_page = NULL;
	}

	return error;
}
/*
 * Package up an I/O request on a vnode into a uio and do it.
 */
int
vn_rdwr(
	enum uio_rw rw,
	struct vnode *vp,
	caddr_t base,
	int len,
	off_t offset,
	enum uio_seg segflg,
	int ioflg,
	kauth_cred_t cred,
	int *aresid,
	proc_t p)
{
	int64_t resid;
	int result;
	
	result = vn_rdwr_64(rw,
			vp,
			(uint64_t)(uintptr_t)base,
			(int64_t)len,
			offset,
			segflg,
			ioflg,
			cred,
			&resid,
			p);

	/* "resid" should be bounded above by "len," which is an int */
	if (aresid != NULL) {
		*aresid = resid;
	}

	return result;
}


int
vn_rdwr_64(
	enum uio_rw rw,
	struct vnode *vp,
	uint64_t base,
	int64_t len,
	off_t offset,
	enum uio_seg segflg,
	int ioflg,
	kauth_cred_t cred,
	int64_t *aresid,
	proc_t p)
{
	uio_t auio;
	int spacetype;
	struct vfs_context context;
	int error=0;
	char uio_buf[ UIO_SIZEOF(1) ];

	context.vc_thread = current_thread();
	context.vc_ucred = cred;

	if (UIO_SEG_IS_USER_SPACE(segflg)) {
		spacetype = proc_is64bit(p) ? UIO_USERSPACE64 : UIO_USERSPACE32;
	}
	else {
		spacetype = UIO_SYSSPACE;
	}
	auio = uio_createwithbuffer(1, offset, spacetype, rw, 
								  &uio_buf[0], sizeof(uio_buf));
	uio_addiov(auio, base, len);

#if CONFIG_MACF
	/* XXXMAC
	 * 	IO_NOAUTH should be re-examined.
 	 *	Likely that mediation should be performed in caller.
	 */
	if ((ioflg & IO_NOAUTH) == 0) {
	/* passed cred is fp->f_cred */
		if (rw == UIO_READ)
			error = mac_vnode_check_read(&context, cred, vp);
		else
			error = mac_vnode_check_write(&context, cred, vp);
	}
#endif

	if (error == 0) {
		if (rw == UIO_READ) {
			if (vnode_isswap(vp)) {
				error = vn_read_swapfile(vp, auio);
			} else {
				error = VNOP_READ(vp, auio, ioflg, &context);
			}
		} else {
			error = VNOP_WRITE(vp, auio, ioflg, &context);
		}
	}

	if (aresid)
		*aresid = uio_resid(auio);
	else
		if (uio_resid(auio) && error == 0)
			error = EIO;
	return (error);
}

/*
 * File table vnode read routine.
 */
static int
vn_read(struct fileproc *fp, struct uio *uio, int flags, vfs_context_t ctx)
{
	struct vnode *vp;
	int error, ioflag;
	off_t count;

	vp = (struct vnode *)fp->f_fglob->fg_data;
	if ( (error = vnode_getwithref(vp)) ) {
		return(error);
	}

#if CONFIG_MACF
	error = mac_vnode_check_read(ctx, vfs_context_ucred(ctx), vp);
	if (error) {
		(void)vnode_put(vp);
		return (error);
	}
#endif

	ioflag = 0;
	if (fp->f_fglob->fg_flag & FNONBLOCK)
		ioflag |= IO_NDELAY;
	if ((fp->f_fglob->fg_flag & FNOCACHE) || vnode_isnocache(vp))
	        ioflag |= IO_NOCACHE;
	if (fp->f_fglob->fg_flag & FNORDAHEAD)
	        ioflag |= IO_RAOFF;

	if ((flags & FOF_OFFSET) == 0)
		uio->uio_offset = fp->f_fglob->fg_offset;
	count = uio_resid(uio);

	if (vnode_isswap(vp)) {
		/* special case for swap files */
		error = vn_read_swapfile(vp, uio);
	} else {
		error = VNOP_READ(vp, uio, ioflag, ctx);
	}
	if ((flags & FOF_OFFSET) == 0)
		fp->f_fglob->fg_offset += count - uio_resid(uio);

	(void)vnode_put(vp);
	return (error);
}


/*
 * File table vnode write routine.
 */
static int
vn_write(struct fileproc *fp, struct uio *uio, int flags, vfs_context_t ctx)
{
	struct vnode *vp;
	int error, ioflag;
	off_t count;
	int clippedsize = 0;
	int partialwrite=0;
	int residcount, oldcount;
	proc_t p = vfs_context_proc(ctx);

	count = 0;
	vp = (struct vnode *)fp->f_fglob->fg_data;
	if ( (error = vnode_getwithref(vp)) ) {
		return(error);
	}

#if CONFIG_MACF
	error = mac_vnode_check_write(ctx, vfs_context_ucred(ctx), vp);
	if (error) {
		(void)vnode_put(vp);
		return (error);
	}
#endif

	ioflag = IO_UNIT;
	if (vp->v_type == VREG && (fp->f_fglob->fg_flag & O_APPEND))
		ioflag |= IO_APPEND;
	if (fp->f_fglob->fg_flag & FNONBLOCK)
		ioflag |= IO_NDELAY;
	if ((fp->f_fglob->fg_flag & FNOCACHE) || vnode_isnocache(vp))
	        ioflag |= IO_NOCACHE;
	/*
	 * Treat synchronous mounts and O_FSYNC on the fd as equivalent.
	 *
	 * XXX We treat O_DSYNC as O_FSYNC for now, since we can not delay
	 * XXX the non-essential metadata without some additional VFS work;
	 * XXX the intent at this point is to plumb the interface for it.
	 */
	if ((fp->f_fglob->fg_flag & (O_FSYNC|O_DSYNC)) ||
		(vp->v_mount && (vp->v_mount->mnt_flag & MNT_SYNCHRONOUS))) {
		ioflag |= IO_SYNC;
	}

	if ((flags & FOF_OFFSET) == 0) {
		uio->uio_offset = fp->f_fglob->fg_offset;
		count = uio_resid(uio);
	}
	if (((flags & FOF_OFFSET) == 0) &&
	 	vfs_context_proc(ctx) && (vp->v_type == VREG) &&
            (((rlim_t)(uio->uio_offset + uio_resid(uio)) > p->p_rlimit[RLIMIT_FSIZE].rlim_cur) ||
             ((rlim_t)uio_resid(uio) > (p->p_rlimit[RLIMIT_FSIZE].rlim_cur - uio->uio_offset)))) {
	     	/*
		 * If the requested residual would cause us to go past the
		 * administrative limit, then we need to adjust the residual
		 * down to cause fewer bytes than requested to be written.  If
		 * we can't do that (e.g. the residual is already 1 byte),
		 * then we fail the write with EFBIG.
		 */
		residcount = uio_resid(uio);
            	if ((rlim_t)(uio->uio_offset + uio_resid(uio)) > p->p_rlimit[RLIMIT_FSIZE].rlim_cur) {
			clippedsize =  (uio->uio_offset + uio_resid(uio)) - p->p_rlimit[RLIMIT_FSIZE].rlim_cur;
		} else if ((rlim_t)uio_resid(uio) > (p->p_rlimit[RLIMIT_FSIZE].rlim_cur - uio->uio_offset)) {
			clippedsize = (p->p_rlimit[RLIMIT_FSIZE].rlim_cur - uio->uio_offset);
		}
		if (clippedsize >= residcount) {
			psignal(p, SIGXFSZ);
			vnode_put(vp);
			return (EFBIG);
		}
		partialwrite = 1;
		uio_setresid(uio, residcount-clippedsize);
	}
	if ((flags & FOF_OFFSET) != 0) {
		/* for pwrite, append should  be ignored */
		ioflag &= ~IO_APPEND;
		if (p && (vp->v_type == VREG) &&
            	((rlim_t)uio->uio_offset  >= p->p_rlimit[RLIMIT_FSIZE].rlim_cur)) {
		psignal(p, SIGXFSZ);
		vnode_put(vp);
		return (EFBIG);
	}
		if (p && (vp->v_type == VREG) &&
			((rlim_t)(uio->uio_offset + uio_resid(uio)) > p->p_rlimit[RLIMIT_FSIZE].rlim_cur)) {
			//Debugger("vn_bwrite:overstepping the bounds");
			residcount = uio_resid(uio);
			clippedsize =  (uio->uio_offset + uio_resid(uio)) - p->p_rlimit[RLIMIT_FSIZE].rlim_cur;
			partialwrite = 1;
			uio_setresid(uio, residcount-clippedsize);
		}
	}

	error = VNOP_WRITE(vp, uio, ioflag, ctx);

	if (partialwrite) {
		oldcount = uio_resid(uio);
		uio_setresid(uio, oldcount + clippedsize);
	}

	if ((flags & FOF_OFFSET) == 0) {
		if (ioflag & IO_APPEND)
			fp->f_fglob->fg_offset = uio->uio_offset;
		else
			fp->f_fglob->fg_offset += count - uio_resid(uio);
	}

	/*
	 * Set the credentials on successful writes
	 */
	if ((error == 0) && (vp->v_tag == VT_NFS) && (UBCINFOEXISTS(vp))) {
		/* 
		 * When called from aio subsystem, we only have the proc from
		 * which to get the credential, at this point, so use that
		 * instead.  This means aio functions are incompatible with
		 * per-thread credentials (aio operations are proxied).  We
		 * can't easily correct the aio vs. settid race in this case
		 * anyway, so we disallow it.
		 */
		if ((flags & FOF_PCRED) == 0) {
			ubc_setthreadcred(vp, p, current_thread());
		} else {
			ubc_setcred(vp, p);
		}
	}
	(void)vnode_put(vp);
	return (error);
}

/*
 * File table vnode stat routine.
 *
 * Returns:	0			Success
 *		EBADF
 *		ENOMEM
 *	vnode_getattr:???
 */
int
vn_stat_noauth(struct vnode *vp, void *sbptr, kauth_filesec_t *xsec, int isstat64, vfs_context_t ctx)
{
	struct vnode_attr va;
	int error;
	u_short mode;
	kauth_filesec_t fsec;
	struct stat *sb = (struct stat *)0;	/* warning avoidance ; protected by isstat64 */
	struct stat64 * sb64 = (struct stat64 *)0;  /* warning avoidance ; protected by isstat64 */

	if (isstat64 != 0)
		sb64 = (struct stat64 *)sbptr;
	else
		sb = (struct stat *)sbptr;
	memset(&va, 0, sizeof(va));
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_fsid);
	VATTR_WANTED(&va, va_fileid);
	VATTR_WANTED(&va, va_mode);
	VATTR_WANTED(&va, va_type);
	VATTR_WANTED(&va, va_nlink);
	VATTR_WANTED(&va, va_uid);
	VATTR_WANTED(&va, va_gid);
	VATTR_WANTED(&va, va_rdev);
	VATTR_WANTED(&va, va_data_size);
	VATTR_WANTED(&va, va_access_time);
	VATTR_WANTED(&va, va_modify_time);
	VATTR_WANTED(&va, va_change_time);
	VATTR_WANTED(&va, va_create_time);
	VATTR_WANTED(&va, va_flags);
	VATTR_WANTED(&va, va_gen);
	VATTR_WANTED(&va, va_iosize);
	/* lower layers will synthesise va_total_alloc from va_data_size if required */
	VATTR_WANTED(&va, va_total_alloc);
	if (xsec != NULL) {
		VATTR_WANTED(&va, va_uuuid);
		VATTR_WANTED(&va, va_guuid);
		VATTR_WANTED(&va, va_acl);
	}
	error = vnode_getattr(vp, &va, ctx);
	if (error)
		goto out;
	/*
	 * Copy from vattr table
	 */
	if (isstat64 != 0) {
		sb64->st_dev = va.va_fsid;
		sb64->st_ino = (ino64_t)va.va_fileid;

	} else {
		sb->st_dev = va.va_fsid;
		sb->st_ino = (ino_t)va.va_fileid;
	}
	mode = va.va_mode;
	switch (vp->v_type) {
	case VREG:
		mode |= S_IFREG;
		break;
	case VDIR:
		mode |= S_IFDIR;
		break;
	case VBLK:
		mode |= S_IFBLK;
		break;
	case VCHR:
		mode |= S_IFCHR;
		break;
	case VLNK:
		mode |= S_IFLNK;
		break;
	case VSOCK:
		mode |= S_IFSOCK;
		break;
	case VFIFO:
		mode |= S_IFIFO;
		break;
	default:
		error = EBADF;
		goto out;
	};
	if (isstat64 != 0) {
		sb64->st_mode = mode;
		sb64->st_nlink = VATTR_IS_SUPPORTED(&va, va_nlink) ? (u_int16_t)va.va_nlink : 1;
		sb64->st_uid = va.va_uid;
		sb64->st_gid = va.va_gid;
		sb64->st_rdev = va.va_rdev;
		sb64->st_size = va.va_data_size;
		sb64->st_atimespec = va.va_access_time;
		sb64->st_mtimespec = va.va_modify_time;
		sb64->st_ctimespec = va.va_change_time;
		sb64->st_birthtimespec = 
				VATTR_IS_SUPPORTED(&va, va_create_time) ? va.va_create_time : va.va_change_time;
		sb64->st_blksize = va.va_iosize;
		sb64->st_flags = va.va_flags;
		sb64->st_blocks = roundup(va.va_total_alloc, 512) / 512;
	} else {
		sb->st_mode = mode;
		sb->st_nlink = VATTR_IS_SUPPORTED(&va, va_nlink) ? (u_int16_t)va.va_nlink : 1;
		sb->st_uid = va.va_uid;
		sb->st_gid = va.va_gid;
		sb->st_rdev = va.va_rdev;
		sb->st_size = va.va_data_size;
		sb->st_atimespec = va.va_access_time;
		sb->st_mtimespec = va.va_modify_time;
		sb->st_ctimespec = va.va_change_time;
		sb->st_blksize = va.va_iosize;
		sb->st_flags = va.va_flags;
		sb->st_blocks = roundup(va.va_total_alloc, 512) / 512;
	}

	/* if we're interested in exended security data and we got an ACL */
	if (xsec != NULL) {
		if (!VATTR_IS_SUPPORTED(&va, va_acl) &&
		    !VATTR_IS_SUPPORTED(&va, va_uuuid) &&
		    !VATTR_IS_SUPPORTED(&va, va_guuid)) {
			*xsec = KAUTH_FILESEC_NONE;
		} else {
		
			if (VATTR_IS_SUPPORTED(&va, va_acl) && (va.va_acl != NULL)) {
				fsec = kauth_filesec_alloc(va.va_acl->acl_entrycount);
			} else {
				fsec = kauth_filesec_alloc(0);
			}
			if (fsec == NULL) {
				error = ENOMEM;
				goto out;
			}
			fsec->fsec_magic = KAUTH_FILESEC_MAGIC;
			if (VATTR_IS_SUPPORTED(&va, va_uuuid)) {
				fsec->fsec_owner = va.va_uuuid;
			} else {
				fsec->fsec_owner = kauth_null_guid;
			}
			if (VATTR_IS_SUPPORTED(&va, va_guuid)) {
				fsec->fsec_group = va.va_guuid;
			} else {
				fsec->fsec_group = kauth_null_guid;
			}
			if (VATTR_IS_SUPPORTED(&va, va_acl) && (va.va_acl != NULL)) {
				bcopy(va.va_acl, &(fsec->fsec_acl), KAUTH_ACL_COPYSIZE(va.va_acl));
			} else {
				fsec->fsec_acl.acl_entrycount = KAUTH_FILESEC_NOACL;
			}
			*xsec = fsec;
		}
	}
	
	/* Do not give the generation number out to unpriviledged users */
	if (va.va_gen && !vfs_context_issuser(ctx)) {
		if (isstat64 != 0)
			sb64->st_gen = 0; 
		else
			sb->st_gen = 0; 
	} else {
		if (isstat64 != 0)
			sb64->st_gen = va.va_gen; 
		else
			sb->st_gen = va.va_gen;
	}

	error = 0;
out:
	if (VATTR_IS_SUPPORTED(&va, va_acl) && va.va_acl != NULL)
		kauth_acl_free(va.va_acl);
	return (error);
}

int
vn_stat(struct vnode *vp, void *sb, kauth_filesec_t *xsec, int isstat64, vfs_context_t ctx)
{
	int error;

#if CONFIG_MACF
	error = mac_vnode_check_stat(ctx, NOCRED, vp);
	if (error)
		return (error);
#endif

	/* authorize */
	if ((error = vnode_authorize(vp, NULL, KAUTH_VNODE_READ_ATTRIBUTES | KAUTH_VNODE_READ_SECURITY, ctx)) != 0)
		return(error);

	/* actual stat */
	return(vn_stat_noauth(vp, sb, xsec, isstat64, ctx));
}


/*
 * File table vnode ioctl routine.
 */
static int
vn_ioctl(struct fileproc *fp, u_long com, caddr_t data, vfs_context_t ctx)
{
	struct vnode *vp = ((struct vnode *)fp->f_fglob->fg_data);
	off_t file_size;
	int error;
	struct vnode *ttyvp;
	int funnel_state;
	struct session * sessp;
	
	if ( (error = vnode_getwithref(vp)) ) {
		return(error);
	}

#if CONFIG_MACF
	error = mac_vnode_check_ioctl(ctx, vp, com);
	if (error)
		goto out;
#endif

	switch (vp->v_type) {
	case VREG:
	case VDIR:
		if (com == FIONREAD) {
			if ((error = vnode_size(vp, &file_size, ctx)) != 0)
				goto out;
			*(int *)data = file_size - fp->f_fglob->fg_offset;
			goto out;
		}
		if (com == FIONBIO || com == FIOASYNC) {	/* XXX */
			goto out;
		}
		/* fall into ... */

	default:
		error = ENOTTY;
		goto out;

	case VFIFO:
	case VCHR:
	case VBLK:

		/* Should not be able to set block size from user space */
		if (com == DKIOCSETBLOCKSIZE) {
			error = EPERM;
			goto out;
		}

		if (com == FIODTYPE) {
			if (vp->v_type == VBLK) {
				if (major(vp->v_rdev) >= nblkdev) {
					error = ENXIO;
					goto out;
				}
				*(int *)data = bdevsw[major(vp->v_rdev)].d_type;

			} else if (vp->v_type == VCHR) {
				if (major(vp->v_rdev) >= nchrdev) {
					error = ENXIO;
					goto out;
				}
				*(int *)data = cdevsw[major(vp->v_rdev)].d_type;
			} else {
				error = ENOTTY;
				goto out;
			}
			goto out;
		}
		error = VNOP_IOCTL(vp, com, data, fp->f_fglob->fg_flag, ctx);

		if (error == 0 && com == TIOCSCTTY) {
			vnode_ref(vp);

			funnel_state = thread_funnel_set(kernel_flock, TRUE);
			sessp = proc_session(vfs_context_proc(ctx));

			session_lock(sessp);
			ttyvp = sessp->s_ttyvp;
			sessp->s_ttyvp = vp;
			sessp->s_ttyvid = vnode_vid(vp);
			session_unlock(sessp);
			session_rele(sessp);
			thread_funnel_set(kernel_flock, funnel_state);

			if (ttyvp)
				vnode_rele(ttyvp);
		}
	}
out:
	(void)vnode_put(vp);
	return(error);
}

/*
 * File table vnode select routine.
 */
static int
vn_select(struct fileproc *fp, int which, void *wql, __unused vfs_context_t ctx)
{
	int error;
	struct vnode * vp = (struct vnode *)fp->f_fglob->fg_data;
	struct vfs_context context;

	if ( (error = vnode_getwithref(vp)) == 0 ) {
		context.vc_thread = current_thread();
		context.vc_ucred = fp->f_fglob->fg_cred;

#if CONFIG_MACF
		/*
		 * XXX We should use a per thread credential here; minimally,
		 * XXX the process credential should have a persistent
		 * XXX reference on it before being passed in here.
		 */
		error = mac_vnode_check_select(ctx, vp, which);
		if (error == 0)
#endif
	        error = VNOP_SELECT(vp, which, fp->f_fglob->fg_flag, wql, ctx);

		(void)vnode_put(vp);
	}
	return(error);
	
}

/*
 * File table vnode close routine.
 */
static int
vn_closefile(struct fileglob *fg, vfs_context_t ctx)
{
	struct vnode *vp = (struct vnode *)fg->fg_data;
	int error;
	struct flock lf;

	if ( (error = vnode_getwithref(vp)) == 0 ) {

		if ((fg->fg_flag & FHASLOCK) && fg->fg_type == DTYPE_VNODE) {
			lf.l_whence = SEEK_SET;
			lf.l_start = 0;
			lf.l_len = 0;
			lf.l_type = F_UNLCK;

			(void)VNOP_ADVLOCK(vp, (caddr_t)fg, F_UNLCK, &lf, F_FLOCK, ctx);
		}
	        error = vn_close(vp, fg->fg_flag, ctx);

		(void)vnode_put(vp);
	}
	return(error);
}

/*
 * Returns:	0			Success
 *	VNOP_PATHCONF:???
 */
int
vn_pathconf(vnode_t vp, int name, int32_t *retval, vfs_context_t ctx)
{
	int	error = 0;

	switch(name) {
	case _PC_EXTENDED_SECURITY_NP:
		*retval = vfs_extendedsecurity(vnode_mount(vp)) ? 1 : 0;
		break;
	case _PC_AUTH_OPAQUE_NP:
		*retval = vfs_authopaque(vnode_mount(vp));
		break;
	case _PC_2_SYMLINKS:
		*retval = 1;	/* XXX NOTSUP on MSDOS, etc. */
		break;
	case _PC_ALLOC_SIZE_MIN:
		*retval = 1;	/* XXX lie: 1 byte */
		break;
	case _PC_ASYNC_IO:	/* unistd.h: _POSIX_ASYNCHRONUS_IO */
		*retval = 1;	/* [AIO] option is supported */
		break;
	case _PC_PRIO_IO:	/* unistd.h: _POSIX_PRIORITIZED_IO */
		*retval = 0;	/* [PIO] option is not supported */
		break;
	case _PC_REC_INCR_XFER_SIZE:
		*retval = 4096;	/* XXX go from MIN to MAX 4K at a time */
		break;
	case _PC_REC_MIN_XFER_SIZE:
		*retval = 4096;	/* XXX recommend 4K minimum reads/writes */
		break;
	case _PC_REC_MAX_XFER_SIZE:
		*retval = 65536; /* XXX recommend 64K maximum reads/writes */
		break;
	case _PC_REC_XFER_ALIGN:
		*retval = 4096;	/* XXX recommend page aligned buffers */
		break;
	case _PC_SYMLINK_MAX:
		*retval = 255;	/* Minimum acceptable POSIX value */
		break;
	case _PC_SYNC_IO:	/* unistd.h: _POSIX_SYNCHRONIZED_IO */
		*retval = 0;	/* [SIO] option is not supported */
		break;
	default:
		error = VNOP_PATHCONF(vp, name, retval, ctx);
		break;
	}

	return (error);
}

static int
vn_kqfilt_add(struct fileproc *fp, struct knote *kn, vfs_context_t ctx)
{
	int error;
	struct vnode *vp;
	
	vp = (struct vnode *)fp->f_fglob->fg_data;

	/*
	 * Don't attach a knote to a dead vnode.
	 */
	if ((error = vget_internal(vp, 0, VNODE_NODEAD)) == 0) {
		switch (kn->kn_filter) {
			case EVFILT_READ:
			case EVFILT_WRITE:
				if (vnode_isfifo(vp)) {
					/* We'll only watch FIFOs that use our fifofs */
					if (!(vp->v_fifoinfo && vp->v_fifoinfo->fi_readsock)) {
						error = ENOTSUP;
					}

				} else if (!vnode_isreg(vp)) {
					if (vnode_isspec(vp) && 
							(error = spec_kqfilter(vp, kn)) == 0) {
						/* claimed by a special device */
						vnode_put(vp);
						return 0;
					}

					error = EINVAL;
				}
				break;
			case EVFILT_VNODE:
				break;
			default:
				error = EINVAL;
		}

		if (error) {
			vnode_put(vp);
			return error;
		}

#if CONFIG_MACF
		error = mac_vnode_check_kqfilter(ctx, fp->f_fglob->fg_cred, kn, vp);
		if (error) {
			vnode_put(vp);
			return error;
		}
#endif

		kn->kn_hook = (void*)vp;
		kn->kn_hookid = vnode_vid(vp);
		kn->kn_fop = &vnode_filtops;

		vnode_lock(vp);
		KNOTE_ATTACH(&vp->v_knotes, kn);
		vnode_unlock(vp);

		/* Ask the filesystem to provide remove notifications, but ignore failure */
		VNOP_MONITOR(vp, 0, VNODE_MONITOR_BEGIN, (void*) kn,  ctx);

		vnode_put(vp);
	}

	return (error);
}

static void
filt_vndetach(struct knote *kn)
{
	vfs_context_t ctx = vfs_context_current();
	struct vnode *vp; 
	vp = (struct vnode *)kn->kn_hook;
	if (vnode_getwithvid(vp, kn->kn_hookid))
		return;

	vnode_lock(vp);
	KNOTE_DETACH(&vp->v_knotes, kn);
	vnode_unlock(vp);
	
	/* 
	 * Tell a (generally networked) filesystem that we're no longer watching 
	 * If the FS wants to track contexts, it should still be using the one from
	 * the VNODE_MONITOR_BEGIN.
	 */
	VNOP_MONITOR(vp, 0, VNODE_MONITOR_END, (void*)kn, ctx);
	vnode_put(vp);
}


/*
 * Used for EVFILT_READ
 *
 * Takes only VFIFO or VREG. vnode is locked.  We handle the "poll" case
 * differently than the regular case for VREG files.  If not in poll(),
 * then we need to know current fileproc offset for VREG.
 */
static intptr_t
vnode_readable_data_count(vnode_t vp, off_t current_offset, int ispoll)
{
	if (vnode_isfifo(vp)) {
		int cnt;
		int err = fifo_charcount(vp, &cnt);
		if (err == 0) {
			return (intptr_t)cnt;
		} else {
			return (intptr_t)0;
		}
	} else if (vnode_isreg(vp)) {
		if (ispoll) {
			return (intptr_t)1;
		}

		off_t amount;
		amount = vp->v_un.vu_ubcinfo->ui_size - current_offset;
		if (amount > (off_t)INTPTR_MAX) {
			return INTPTR_MAX;
		} else if (amount < (off_t)INTPTR_MIN) {
			return INTPTR_MIN;
		} else {
			return (intptr_t)amount;
		} 
	} else {
		panic("Should never have an EVFILT_READ except for reg or fifo.");
		return 0;
	}
}

/*
 * Used for EVFILT_WRITE.  
 *
 * For regular vnodes, we can always write (1).  For named pipes,
 * see how much space there is in the buffer.  Nothing else is covered.
 */
static intptr_t
vnode_writable_space_count(vnode_t vp) 
{
	if (vnode_isfifo(vp)) {
		long spc;
		int err = fifo_freespace(vp, &spc);
		if (err == 0) {
			return (intptr_t)spc;
		} else {
			return (intptr_t)0;
		}
	} else if (vnode_isreg(vp)) {
		return (intptr_t)1;
	} else {
		panic("Should never have an EVFILT_READ except for reg or fifo.");
		return 0;
	}
}

/* 
 * Determine whether this knote should be active
 * 
 * This is kind of subtle.  
 * 	--First, notice if the vnode has been revoked: in so, override hint
 * 	--EVFILT_READ knotes are checked no matter what the hint is
 * 	--Other knotes activate based on hint.  
 * 	--If hint is revoke, set special flags and activate
 */
static int
filt_vnode(struct knote *kn, long hint)
{
	struct vnode *vp = (struct vnode *)kn->kn_hook;
	int activate = 0;

	if (0 == hint) {
		if ((vnode_getwithvid(vp, kn->kn_hookid) != 0)) {
			hint = NOTE_REVOKE;
		} else {
			vnode_put(vp);
		}
	}    

	/* NOTE_REVOKE is special, as it is only sent during vnode reclaim */
	if (NOTE_REVOKE == hint) {
		kn->kn_flags |= (EV_EOF | EV_ONESHOT);
		activate = 1;

		if ((kn->kn_filter == EVFILT_VNODE) && (kn->kn_sfflags & NOTE_REVOKE)) {
			kn->kn_fflags |= NOTE_REVOKE;
		}
	} else {
		switch(kn->kn_filter) {
			case EVFILT_READ:
				kn->kn_data = vnode_readable_data_count(vp, kn->kn_fp->f_fglob->fg_offset, (kn->kn_flags & EV_POLL));

				if (kn->kn_data != 0) {
					activate = 1;
				}
				break;
			case EVFILT_WRITE: 
				kn->kn_data = vnode_writable_space_count(vp);

				if (kn->kn_data != 0) {
					activate = 1;
				}
				break;
			case EVFILT_VNODE:
				/* Check events this note matches against the hint */
				if (kn->kn_sfflags & hint) {
					kn->kn_fflags |= hint; /* Set which event occurred */
				}
				if (kn->kn_fflags != 0) {
					activate = 1;
				}
				break;
			default:
				panic("Invalid knote filter on a vnode!\n");
		}
	}

	return (activate);
}
