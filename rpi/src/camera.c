/* 

    P2P Food Lab Sensorbox

    Copyright (C) 2013  Sony Computer Science Laboratory Paris
    Author: Peter Hanappe

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

/*

  camera.c is based on Tobias Müller's v4l2grab Version 0.1. Original
  copyright below:

*/

/***************************************************************************
 *   v4l2grab Version 0.1                                                  *
 *   Copyright (C) 2009 by Tobias Müller                                   *
 *   Tobias_Mueller@twam.info                                              *
 *                                                                         *
 *   based on V4L2 Specification, Appendix B: Video Capture Example        *
 *   (http://v4l2spec.bytesex.org/spec/capture-example.html)               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#define _BSD_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <asm/types.h>
#include <linux/videodev2.h>
#include <jpeglib.h>
#include "log_message.h"
#include "camera.h"

#if !defined(IO_READ) && !defined(IO_MMAP) && !defined(IO_USERPTR)
#error You have to include one of IO_READ, IO_MMAP oder IO_USERPTR!
#endif

#define BLOCKSIZE 4096
#define CLEAR(x) memset (&(x), 0, sizeof (x))

typedef struct _buffer_t {
        void * start;
        size_t length;
} buffer_t;

enum {
        CAMERA_CLEAN,
        CAMERA_OPEN,
        CAMERA_INIT,
        CAMERA_CAPTURING,
        CAMERA_ERROR
};

struct _camera_t {
        io_method io;
        int fd;
        int debug;
        /* struct video_capability vid_cap; */
        /* struct video_picture vid_pic; */
        /* struct video_window vid_win; */
        char* device_name;
        unsigned int width;
        unsigned int height;
        unsigned char jpeg_quality;
        buffer_t* buffers;
        unsigned int n_buffers;
        unsigned char* rgb_buffer;
        int rgb_buffer_size;
        unsigned char* jpeg_buffer;
        int jpeg_buffer_size;
        int image_size;
        int state;
};

static int camera_prepare(camera_t* camera);
static int camera_open(camera_t* camera);
static int camera_init(camera_t* camera);
static int camera_capturestart(camera_t* camera);
static int camera_capturestop(camera_t* camera);
static int camera_close(camera_t* camera);
static int camera_cleanup(camera_t* camera);
static int camera_read(camera_t* camera);
static int camera_convert(camera_t* camera, void* p);
static int camera_converttojpeg(camera_t* camera);

static void convert_yuv422_to_rgb888(int width, int height, unsigned char *src, unsigned char *dst);
static void jpeg_bufferinit(j_compress_ptr cinfo);
static boolean jpeg_bufferemptyoutput(j_compress_ptr cinfo);
static void jpeg_bufferterminate(j_compress_ptr cinfo);

#ifdef IO_READ
static int camera_readinit(camera_t* camera, unsigned int buffer_size);
#endif

#ifdef IO_MMAP
static int camera_mmapinit(camera_t* camera);
#endif

#ifdef IO_USERPTR
static int camera_userptrinit(camera_t* camera_t, unsigned int buffer_size);
#endif


/* static void print_palette(int p); */
/* static void camera_cap(camera_t* camera); */
/* static void camera_get_pic_info(camera_t* camera); */
/* static void camera_get_win_info(camera_t* camera); */

camera_t* new_camera(const char* dev, 
                     io_method io,
                     unsigned int width, 
                     unsigned int height, 
                     int jpeg_quality)
{
        camera_t* camera = (camera_t*) malloc(sizeof(camera_t));
        if (camera == NULL) {
                log_err("Camera: Out of memory");
                return NULL;
        }
        memset(camera, 0, sizeof(camera_t));
        
        camera->io = io;
        camera->fd = -1;
        camera->debug = 1;
        camera->device_name = strdup(dev);
        camera->width = width;
        camera->height = height;
        camera->jpeg_quality = jpeg_quality;
        camera->buffers = NULL;
        camera->n_buffers = 0;
        camera->rgb_buffer = NULL;
        camera->rgb_buffer_size = 0;
        camera->jpeg_buffer = NULL;
        camera->jpeg_buffer_size = 0;
        camera->image_size = 0;
        camera->state = CAMERA_CLEAN;

        return camera;
}

int delete_camera(camera_t* camera)
{
        if (camera) {
                camera_capturestop(camera);
                camera_close(camera);
                camera_cleanup(camera);
                free(camera);
        }
        return 0;
}

static int camera_prepare(camera_t* camera)
{
        if (camera->state == CAMERA_ERROR)
                return -1;

        if ((camera->state == CAMERA_CLEAN) 
            && (camera_open(camera) != 0)) {
                camera->state = CAMERA_ERROR;
                return -1;
        }

        if ((camera->state == CAMERA_OPEN) 
            && (camera_init(camera) != 0)) {
                camera->state = CAMERA_ERROR;
                camera_close(camera);
                camera_cleanup(camera);
                return -1;
        }        

        if ((camera->state == CAMERA_INIT) 
            && (camera_capturestart(camera) != 0)) {
                camera->state = CAMERA_ERROR;
                camera_close(camera);
                camera_cleanup(camera);
                return -1;
        }

        return 0;
}

int camera_capture(camera_t* camera)
{
        int error, again;

        if ((camera->state != CAMERA_CAPTURING) 
            && (camera_prepare(camera) != 0)) {
                return -1;
        }
        
        for (again = 0; again < 10; again++) {
                fd_set fds;
                struct timeval tv;
                int r;

                FD_ZERO(&fds);
                FD_SET(camera->fd, &fds);

                /* Timeout. */
                tv.tv_sec = 2;
                tv.tv_usec = 0;

                r = select(camera->fd + 1, &fds, NULL, NULL, &tv);

                if (-1 == r) {
                        if (EINTR == errno)
                                continue;

                        log_err("Camera: select error %d, %s", errno, strerror(errno));
                        return -1;
                }

                if (0 == r) {
                        log_err("Camera: select timeout");
                        continue;
                }
                        
                error = camera_read(camera);
                if (error == 0) 
                        break;

                // else: EAGAIN - continue select loop. */
        }

        if (error) {
                log_err("Camera: Failed to grab frame");
                return -1;
        }

        return 0;
}

int camera_getimagesize(camera_t* camera)
{
        return camera->image_size;
}

unsigned char* camera_getimagebuffer(camera_t* camera)
{
        return camera->jpeg_buffer;
}

/**
   Convert from YUV422 format to RGB888. Formulae are described on http://en.wikipedia.org/wiki/YUV

   \param width width of image
   \param height height of image
   \param src source
   \param dst destination
*/
void convert_yuv422_to_rgb888(int width, int height, unsigned char *src, unsigned char *dst)
{
        int line, column;
        unsigned char *py, *pu, *pv;
        unsigned char *tmp = dst;

        /* In this format each four bytes is two pixels. Each four bytes is two Y's, a Cb and a Cr. 
           Each Y goes to one of the pixels, and the Cb and Cr belong to both pixels. */
        py = src;
        pu = src + 1;
        pv = src + 3;

#define CLIP(x) ( (x)>=0xFF ? 0xFF : ( (x) <= 0x00 ? 0x00 : (x) ) )

        for (line = 0; line < height; ++line) {
                for (column = 0; column < width; ++column) {
                        *tmp++ = CLIP((double)*py + 1.402*((double)*pv-128.0));
                        *tmp++ = CLIP((double)*py - 0.344*((double)*pu-128.0) - 0.714*((double)*pv-128.0));      
                        *tmp++ = CLIP((double)*py + 1.772*((double)*pu-128.0));

                        // increase py every time
                        py += 2;
                        // increase pu,pv every second time
                        if ((column & 1)==1) {
                                pu += 4;
                                pv += 4;
                        }
                }
        }
}

/**
   Do ioctl and retry if error was EINTR ("A signal was caught during the ioctl() operation."). Parameters are the same as on ioctl.

   \param fd file descriptor
   \param request request
   \param argp argument
   \returns result from ioctl
*/
int xioctl(int fd, int request, void* argp)
{
        int r;
        do {
                r = ioctl(fd, request, argp);
        } while (-1 == r && EINTR == errno);

        return r;
}


typedef struct _jpeg_my_dest_mgr_t {
        struct jpeg_destination_mgr mgr;
        camera_t* camera;
} jpeg_my_dest_mgr_t;


static void jpeg_bufferinit(j_compress_ptr cinfo)
{
        jpeg_my_dest_mgr_t* my_mgr = (jpeg_my_dest_mgr_t*) cinfo->dest;
        camera_t* camera = my_mgr->camera;

        cinfo->dest->next_output_byte = camera->jpeg_buffer;
        cinfo->dest->free_in_buffer = camera->jpeg_buffer_size;
}

static boolean jpeg_bufferemptyoutput(j_compress_ptr cinfo)
{
        jpeg_my_dest_mgr_t* my_mgr = (jpeg_my_dest_mgr_t*) cinfo->dest;
        camera_t* camera = my_mgr->camera;
        
        int oldsize = camera->jpeg_buffer_size;
        camera->jpeg_buffer = (unsigned char*) realloc(camera->jpeg_buffer, 
                                                       camera->jpeg_buffer_size + BLOCKSIZE);
        if (camera->jpeg_buffer == NULL) {
                log_err("Camera: Out of memory");
                return 0;
        }
        camera->jpeg_buffer_size += BLOCKSIZE;
        cinfo->dest->next_output_byte = &camera->jpeg_buffer[oldsize];
        cinfo->dest->free_in_buffer = BLOCKSIZE;

        return 1;
}

static void jpeg_bufferterminate(j_compress_ptr cinfo)
{
        jpeg_my_dest_mgr_t* my_mgr = (jpeg_my_dest_mgr_t*) cinfo->dest;
        camera_t* camera = my_mgr->camera;
        camera->image_size = camera->jpeg_buffer_size - cinfo->dest->free_in_buffer;
}

static int camera_converttojpeg(camera_t* camera)
{
        struct jpeg_compress_struct cinfo;
        struct jpeg_error_mgr jerr;
	jpeg_my_dest_mgr_t* my_mgr;

        JSAMPROW row_pointer[1];

        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_compress(&cinfo);

        cinfo.dest = (struct jpeg_destination_mgr *) 
                (*cinfo.mem->alloc_small) ((j_common_ptr) &cinfo, JPOOL_PERMANENT,
                                           sizeof(jpeg_my_dest_mgr_t));       
        cinfo.dest->init_destination = &jpeg_bufferinit;
        cinfo.dest->empty_output_buffer = &jpeg_bufferemptyoutput;
        cinfo.dest->term_destination = &jpeg_bufferterminate;

        my_mgr = (jpeg_my_dest_mgr_t*) cinfo.dest;
        my_mgr->camera = camera;

        cinfo.image_width = camera->width;	
        cinfo.image_height = camera->height;
        cinfo.input_components = 3;
        cinfo.in_color_space = JCS_RGB;

        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, camera->jpeg_quality, TRUE);

        jpeg_start_compress(&cinfo, TRUE);

        // feed data
        while (cinfo.next_scanline < cinfo.image_height) {
                row_pointer[0] = &camera->rgb_buffer[cinfo.next_scanline * cinfo.image_width *  cinfo.input_components];
                jpeg_write_scanlines(&cinfo, row_pointer, 1);
        }

        jpeg_finish_compress(&cinfo);

        jpeg_destroy_compress(&cinfo);

        return 0;
}

/**
   process image read
*/
static int camera_convert(camera_t* camera, void* src)
{
        int size = camera->width * camera->height * 3;
        if ((camera->rgb_buffer_size != size) 
            && (camera->rgb_buffer != NULL)) {
                free(camera->rgb_buffer);
                camera->rgb_buffer = NULL;
        }
        if (camera->rgb_buffer == NULL) {
                camera->rgb_buffer = malloc(size);
                if (camera->rgb_buffer == NULL) {
                        log_err("Camera: Out of memory");
                        return -1;
                }
                camera->rgb_buffer_size = size;
        }

        convert_yuv422_to_rgb888(camera->width, camera->height, src, camera->rgb_buffer);

        if (camera->jpeg_buffer == NULL) {
                camera->jpeg_buffer = (unsigned char*) realloc(camera->jpeg_buffer, BLOCKSIZE);
                if (camera->jpeg_buffer == NULL) {
                        log_err("Camera: Out of memory");
                        return -1;
                }
                camera->jpeg_buffer_size = BLOCKSIZE;
        }

        if (camera_converttojpeg(camera) != 0)
                return -1;

        return 0;
}

static int camera_read(camera_t* camera)
{
        struct v4l2_buffer buf;
#ifdef IO_USERPTR
        unsigned int i;
#endif

        switch (camera->io) {
#ifdef IO_READ
        case IO_METHOD_READ:
                if (-1 == read (camera->fd, camera->buffers[0].start, camera->buffers[0].length)) {
                        switch (errno) {
                        case EAGAIN:
                                return 1;

                        case EIO:
                                // Could ignore EIO, see spec.

                                // fall through
                        default:
                                log_err("Camera: read error %d, %s", errno, strerror(errno));
                                return -1;
                        }
                }

                camera_convert(camera, camera->buffers[0].start);
                break;
#endif

#ifdef IO_MMAP
        case IO_METHOD_MMAP:
                CLEAR(buf);

                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_MMAP;

                if (-1 == xioctl(camera->fd, VIDIOC_DQBUF, &buf)) {
                        switch (errno) {
                        case EAGAIN:
                                return 1;

                        case EIO:
                                // Could ignore EIO, see spec

                                // fall through
                        default:
                                log_err("Camera: VIDIOC_DQBUF error %d, %s", errno, strerror(errno));
                                return -1;
                        }
                }

                assert(buf.index < camera->n_buffers);

                camera_convert(camera, camera->buffers[buf.index].start);

                if (-1 == xioctl(camera->fd, VIDIOC_QBUF, &buf)) {
                        log_err("Camera: VIDIOC_QBUF error %d, %s", errno, strerror(errno));
                        return -1;
                }

                break;
#endif

#ifdef IO_USERPTR
        case IO_METHOD_USERPTR:
                CLEAR(buf);

                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_USERPTR;

                if (-1 == xioctl(camera->fd, VIDIOC_DQBUF, &buf)) {
                        switch (errno) {
                        case EAGAIN:
                                return 1;

                        case EIO:
                                // Could ignore EIO, see spec.

                                // fall through
                        default:
                                log_err("Camera: VIDIOC_DQBUF error %d, %s", errno, strerror(errno));
                                return -1;
                        }
                }

                for (i = 0; i < camera->n_buffers; ++i)
                        if ((buf.m.userptr == (unsigned long) camera->buffers[i].start) 
                            && (buf.length == camera->buffers[i].length))
                                break;

                assert(i < camera->n_buffers);

                camera_convert(camera, (void *) buf.m.userptr);

                if (-1 == xioctl(camera->fd, VIDIOC_QBUF, &buf)) {
                        log_err("Camera: VIDIOC_QBUF error %d, %s", errno, strerror(errno));
                        return -1;
                }
                break;
#endif
        }

        return 0;
}

static int camera_capturestop(camera_t* camera)
{
        enum v4l2_buf_type type;

        switch (camera->io) {
#ifdef IO_READ
        case IO_METHOD_READ:
                /* Nothing to do. */
                break;
#endif

#ifdef IO_MMAP
        case IO_METHOD_MMAP:
#endif
#ifdef IO_USERPTR
        case IO_METHOD_USERPTR:
#endif
#if defined(IO_MMAP) || defined(IO_USERPTR)
                type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                
                if (camera->fd == -1)
                        return 0;

                if (-1 == xioctl(camera->fd, VIDIOC_STREAMOFF, &type)) {
                        log_err("Camera: VIDIOC_STREAMOFF error %d, %s", errno, strerror(errno));
                        return -1;
                }

                break;
#endif 
        }

        return 0;
}

static int camera_capturestart(camera_t* camera)
{
        unsigned int i;
        enum v4l2_buf_type type;

        if (camera->state != CAMERA_INIT) {
                log_err("Camera: Not in the init state");
                return -1;
        }

        switch (camera->io) {
#ifdef IO_READ    
        case IO_METHOD_READ:
                /* Nothing to do. */
                break;
#endif

#ifdef IO_MMAP
        case IO_METHOD_MMAP:
                for (i = 0; i < camera->n_buffers; ++i) {
                        struct v4l2_buffer buf;

                        CLEAR(buf);

                        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                        buf.memory = V4L2_MEMORY_MMAP;
                        buf.index = i;

                        if (-1 == xioctl(camera->fd, VIDIOC_QBUF, &buf)) {
                                log_err("Camera: VIDIOC_QBUF error %d, %s", errno, strerror(errno));
                                return -1;
                        }
                }
                
                type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

                if (-1 == xioctl(camera->fd, VIDIOC_STREAMON, &type)) {
                        log_err("Camera: VIDIOC_STREAMON error %d, %s", errno, strerror(errno));
                        return -1;
                }

                break;
#endif

#ifdef IO_USERPTR
        case IO_METHOD_USERPTR:
                for (i = 0; i < camera->n_buffers; ++i) {
                        struct v4l2_buffer buf;

                        CLEAR(buf);

                        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                        buf.memory = V4L2_MEMORY_USERPTR;
                        buf.index = i;
                        buf.m.userptr = (unsigned long) camera->buffers[i].start;
                        buf.length = camera->buffers[i].length;

                        if (-1 == xioctl(camera->fd, VIDIOC_QBUF, &buf)) {
                                log_err("Camera: VIDIOC_QBUF error %d, %s", errno, strerror(errno));
                                return -1;
                        }
                }

                type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

                if (-1 == xioctl(camera->fd, VIDIOC_STREAMON, &type)) {
                        log_err("Camera: VIDIOC_STREAMON error %d, %s", errno, strerror(errno));
                        return -1;
                }

                break;
#endif
        }

        camera->state = CAMERA_CAPTURING;

        return 0;
}

static int camera_cleanup(camera_t* camera)
{
        unsigned int i;

        if (camera == NULL)
                return 0;

        switch (camera->io) {
#ifdef IO_READ
        case IO_METHOD_READ:
                if ((camera->n_buffers > 0) && camera->buffers)
                        free(camera->buffers[0].start);
                break;
#endif

#ifdef IO_MMAP
        case IO_METHOD_MMAP:
                for (i = 0; i < camera->n_buffers; ++i)
                        if (-1 == munmap(camera->buffers[i].start, camera->buffers[i].length)) {
                                log_err("Camera: munmap error %d, %s", errno, strerror(errno));
                                return -1;
                        }
                break;
#endif

#ifdef IO_USERPTR
        case IO_METHOD_USERPTR:
                for (i = 0; i < camera->n_buffers; ++i)
                        free(camera->buffers[i].start);
                break;
#endif
        }
        
        if (camera->buffers)
                free(camera->buffers);

        if (camera->device_name != NULL)
                free(camera->device_name);

        if (camera->rgb_buffer != NULL) 
                free(camera->rgb_buffer);

        if (camera->jpeg_buffer != NULL) 
                free(camera->jpeg_buffer);

        return 0;
}

#ifdef IO_READ
static int camera_readinit(camera_t* camera, unsigned int buffer_size)
{
        camera->n_buffers = 1;
        camera->buffers = calloc(1, sizeof(buffer_t));
        if (!camera->buffers) {
                log_err("Camera: Out of memory");
                return -1;
        }

        camera->buffers[0].length = buffer_size;
        camera->buffers[0].start = malloc(buffer_size);
        if (!camera->buffers[0].start) {
                log_err("Camera: Out of memory");
                return -1;
        }

        return 0;
}
#endif

#ifdef IO_MMAP
static int camera_mmapinit(camera_t* camera)
{
        struct v4l2_requestbuffers req;

        CLEAR(req);

        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (-1 == xioctl(camera->fd, VIDIOC_REQBUFS, &req)) {
                if (EINVAL == errno) {
                        log_err("Camera: %s does not support memory mapping", camera->device_name);
                        return -1;
                } else {
                        log_err("Camera: VIDIOC_REQBUFS error %d, %s", errno, strerror(errno));
                        return -1;
                }
        }

        if (req.count < 2) {
                log_err("Camera: Insufficient buffer memory on %s", camera->device_name);
                return -1;
        }

        camera->buffers = calloc(req.count, sizeof(buffer_t));
        if (!camera->buffers) {
                log_err("Camera: Out of memory");
                return -1;
        }

        for (camera->n_buffers = 0; camera->n_buffers < req.count; camera->n_buffers++) {
                struct v4l2_buffer buf;

                CLEAR(buf);

                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_MMAP;
                buf.index = camera->n_buffers;

                if (-1 == xioctl(camera->fd, VIDIOC_QUERYBUF, &buf)) {
                        log_err("Camera: VIDIOC_QUERYBUF error %d, %s", errno, strerror(errno));
                        return -1;
                }
                camera->buffers[camera->n_buffers].length = buf.length;
                camera->buffers[camera->n_buffers].start = mmap(NULL /* start anywhere */, 
                                                                buf.length, 
                                                                PROT_READ | PROT_WRITE /* required */, 
                                                                MAP_SHARED /* recommended */, 
                                                                camera->fd, buf.m.offset);

                if (MAP_FAILED == camera->buffers[camera->n_buffers].start) {
                        log_err("Camera: mmap error %d, %s", errno, strerror(errno));
                        return -1;
                }
        }

        return 0;
}
#endif

#ifdef IO_USERPTR
static int camera_userptrinit(camera_t* camera, unsigned int buffer_size)
{
        struct v4l2_requestbuffers req;
        unsigned int page_size;

        page_size = getpagesize ();
        buffer_size = (buffer_size + page_size - 1) & ~(page_size - 1);

        CLEAR(req);

        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_USERPTR;

        if (-1 == xioctl(camera->fd, VIDIOC_REQBUFS, &req)) {
                if (EINVAL == errno) {
                        log_err("Camera: %s does not support user pointer i/o", camera->device_name);
                        return -1;
                } else {
                        log_err("Camera: VIDIOC_REQBUFS error %d, %s", errno, strerror(errno));
                        return -1;
                }
        }

        camera->buffers = calloc(4, sizeof(buffer_t));

        if (!camera->buffers) {
                log_err("Camera: Out of memory");
                return -1;
        }

        for (camera->n_buffers = 0; camera->n_buffers < 4; camera->n_buffers++) {
                camera->buffers[camera->n_buffers].length = buffer_size;
                camera->buffers[camera->n_buffers].start = memalign (/* boundary */ page_size, buffer_size);

                if (!camera->buffers[camera->n_buffers].start) {
                        log_err("Camera: Out of memory");
                        return -1;
                }
        }

        return 0;
}
#endif

static int camera_list_devices(camera_t* camera, char** name, int* count)
{
        char path[256];
        struct stat st;
        int n = 0;

        for (int i = 0; i <= 9; i++) {
                snprintf(path, sizeof(path), "/dev/video%d", i);
                if ((stat(path, &st) == 0) && S_ISCHR(st.st_mode))
                        name[n++] = strdup(path); 
        }
        *count = n;
        return 0;
}

static int camera_open_dev(camera_t* camera, const char* name)
{
        struct stat st;

        if (camera->state != CAMERA_CLEAN) {
                log_err("Camera: Not in the clean state");
                return -1;
        }

        // stat file
        if (stat(name, &st) == -1) {
                log_err("Camera: Cannot identify '%s': %d, %s", name, errno, strerror(errno));
                return -1;
        }

        // check if its device
        if (!S_ISCHR(st.st_mode)) {
                log_err("Camera: %s is no device", name);
                return -1;
        }

        // open device
        camera->fd = open(name, O_RDWR /* required */ | O_NONBLOCK, 0);

        // check if opening was successfull
        if (-1 == camera->fd) {
                log_err("Camera: Cannot open '%s': %d, %s", name, errno, strerror(errno));
                return -1;
        }

        camera->state = CAMERA_OPEN;


        return 0;
}

static int camera_open(camera_t* camera)
{
        int ret = camera_open_dev(camera, camera->device_name);
        if (ret == 0)
                return 0;

        log_warn("Camera: Failed to open default device (%s). Looking for another device.", 
                 camera->device_name);
        
        char* list[10];
        int count = 0;

        for (int i = 0; i < 10; i++)
                list[i] = NULL;

        camera_list_devices(camera, list, &count);
        ret = -1;
        for (int i = 0; i < count; i++) {
                ret = camera_open_dev(camera, list[i]);
                if (ret == 0) {
                        log_info("Camera: Using %s for camera.", list[i]);
                        break;
                }
        }
        for (int i = 0; i < count; i++)
                free(list[i]);

        return ret;
}

/*
static int camera_setctrl(camera_t* camera, int id, int value)
{
        struct v4l2_queryctrl queryctrl;
        struct v4l2_control control;

        memset(&queryctrl, 0, sizeof(queryctrl));
        queryctrl.id = id;

        if (-1 == ioctl(camera->fd, VIDIOC_QUERYCTRL, &queryctrl)) {
                if (errno != EINVAL) {
                        log_err("Camera: VIDIOC_QUERYCTRL: error %d, %s", errno, strerror(errno));
                        return -1;
                } else {
                        log_warn("Camera: control %d is not supported", id);
                }
        } else if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED) {
                log_warn("Camera: Control %d is not supported", id);
        } else {
                memset(&control, 0, sizeof (control));
                control.id = id;
                control.value = value;
                if (-1 == ioctl(camera->fd, VIDIOC_S_CTRL, &control)) {
                        log_err("Camera: VIDIOC_S_CTRL: error %d, %s", errno, strerror(errno));
                        return -1;
                }
        }
        return 0;
}
*/

static int camera_init(camera_t* camera)
{
        struct v4l2_capability cap;
        struct v4l2_cropcap cropcap;
        struct v4l2_crop crop;
        struct v4l2_format fmt;
        unsigned int min;

        if (camera->state != CAMERA_OPEN) {
                log_err("Camera: Not in the open state");
                return -1;
        }

        if (-1 == xioctl(camera->fd, VIDIOC_QUERYCAP, &cap)) {
                if (EINVAL == errno) {
                        log_err("Camera: %s is no V4L2 device", camera->device_name);
                        return -1;
                } else {
                        log_err("Camera: VIDIOC_QUERYCAP error %d, %s", errno, strerror(errno));
                        return -1;
                }
        }

        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
                log_err("Camera: %s is no video capture device", camera->device_name);
                return -1;
        }

        switch (camera->io) {
#ifdef IO_READ
        case IO_METHOD_READ:
                if (!(cap.capabilities & V4L2_CAP_READWRITE)) {
                        log_err("Camera: %s does not support read i/o", camera->device_name);
                        return -1;
                }
                break;
#endif

#ifdef IO_MMAP
        case IO_METHOD_MMAP:
#endif
#ifdef IO_USERPTR
        case IO_METHOD_USERPTR:
#endif
#if defined(IO_MMAP) || defined(IO_USERPTR)
                if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
                        log_err("Camera: %s does not support streaming i/o", camera->device_name);
                        return -1;
                }
                break;
#endif
        }


        /* Select video input, video standard and tune here. */
        CLEAR(cropcap);

        cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (0 == xioctl(camera->fd, VIDIOC_CROPCAP, &cropcap)) {
                crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                crop.c = cropcap.defrect; /* reset to default */

                if (-1 == xioctl(camera->fd, VIDIOC_S_CROP, &crop)) {
                        switch (errno) {
                        case EINVAL:
                                /* Cropping not supported. */
                                break;
                        default:
                                /* Errors ignored. */
                                break;
                        }
                }
        } else {        
                /* Errors ignored. */
        }

        CLEAR(fmt);

        log_info("Camera: Opening video device %dx%d.", camera->width, camera->height);

        // v4l2_format
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = camera->width; 
        fmt.fmt.pix.height = camera->height;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

        if (-1 == xioctl(camera->fd, VIDIOC_S_FMT, &fmt)) {
                log_err("Camera: VIDIOC_S_FMT error %d, %s", errno, strerror(errno));
                return -1;
        }

        /* Note VIDIOC_S_FMT may change width and height. */
        if (camera->width != fmt.fmt.pix.width) {
                camera->width = fmt.fmt.pix.width;
                log_err("Camera: Image width set to %i by device %s.", camera->width, camera->device_name);
        }
        if (camera->height != fmt.fmt.pix.height) {
                camera->height = fmt.fmt.pix.height;
                log_err("Camera: Image height set to %i by device %s.", camera->height, camera->device_name);
        }

        /* Buggy driver paranoia. */
        min = fmt.fmt.pix.width * 2;
        if (fmt.fmt.pix.bytesperline < min)
                fmt.fmt.pix.bytesperline = min;
        min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
        if (fmt.fmt.pix.sizeimage < min)
                fmt.fmt.pix.sizeimage = min;

        switch (camera->io) {
#ifdef IO_READ
        case IO_METHOD_READ:
                if (camera_readinit(camera, fmt.fmt.pix.sizeimage) != 0)
                        return -1;
                break;
#endif

#ifdef IO_MMAP
        case IO_METHOD_MMAP:
                if (camera_mmapinit(camera) != 0)
                        return -1;
                break;
#endif

#ifdef IO_USERPTR
        case IO_METHOD_USERPTR:
                if (camera_userptrinit(camera, fmt.fmt.pix.sizeimage) != 0)
                        return -1;
                break;
#endif
        }

        /* camera_cap(camera); */
        /* camera_get_pic_info(camera); */
        /* camera_get_win_info(camera); */

        //camera_setctrl(camera, V4L2_CID_CONTRAST, 127);
        //camera_setctrl(camera, V4L2_CID_BRIGHTNESS, 127);
        //camera_setctrl(camera, V4L2_CID_HUE, 127);
        /* camera_setctrl(camera, V4L2_CID_GAMMA, 127); */
        /* camera_setctrl(camera, V4L2_CID_RED_BALANCE, 127); */
        /* camera_setctrl(camera, V4L2_CID_BLUE_BALANCE, 127); */
        /* camera_setctrl(camera, V4L2_CID_EXPOSURE, 127); */

        /*
        struct v4l2_queryctrl queryctrl;
        struct v4l2_control control;

        memset(&queryctrl, 0, sizeof (queryctrl));
        queryctrl.id = V4L2_CID_BRIGHTNESS;

        if (-1 == ioctl(camera->fd, VIDIOC_QUERYCTRL, &queryctrl)) {
                if (errno != EINVAL) {
                        log_err("Camera: VIDIOC_QUERYCTRL: error %d, %s", errno, strerror(errno));
                        return -1;
                } else {
                        log_warn("Camera: V4L2_CID_BRIGHTNESS is not supported\n");
                }
        } else if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED) {
                log_warn("Camera: V4L2_CID_BRIGHTNESS is not supported\n");
        } else {
                memset(&control, 0, sizeof (control));
                control.id = V4L2_CID_BRIGHTNESS;
                control.value = queryctrl.default_value;

                if (-1 == ioctl(camera->fd, VIDIOC_S_CTRL, &control)) {
                        log_err("Camera: VIDIOC_S_CTRL: error %d, %s", errno, strerror(errno));
                        return -1;
                }
        }


        memset(&queryctrl, 0, sizeof (queryctrl));
        queryctrl.id = V4L2_CID_AUTO_WHITE_BALANCE;

        if (-1 == ioctl(camera->fd, VIDIOC_QUERYCTRL, &queryctrl)) {
                if (errno != EINVAL) {
                        log_err("Camera: VIDIOC_QUERYCTRL: error %d, %s", errno, strerror(errno));
                        return -1;
                } else {
                        log_warn("Camera: V4L2_CID_AUTO_WHITE_BALANCE is not supported\n");
                }
        } else if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED) {
                log_warn("Camera: V4L2_CID_AUTO_WHITE_BALANCE is not supported\n");
        } else {
                memset(&control, 0, sizeof (control));
                control.id = V4L2_CID_AUTO_WHITE_BALANCE;
                control.value = 1;

                if (-1 == ioctl(camera->fd, VIDIOC_S_CTRL, &control)) {
                        log_err("Camera: VIDIOC_S_CTRL: error %d, %s", errno, strerror(errno));
                        return -1;
                }
        }


        memset(&queryctrl, 0, sizeof (queryctrl));
        queryctrl.id = V4L2_CID_AUTOGAIN;

        if (-1 == ioctl(camera->fd, VIDIOC_QUERYCTRL, &queryctrl)) {
                if (errno != EINVAL) {
                        log_err("Camera: VIDIOC_QUERYCTRL: error %d, %s", errno, strerror(errno));
                        return -1;
                } else {
                        log_warn("Camera: V4L2_CID_AUTOGAIN is not supported\n");
                }
        } else if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED) {
                log_warn("Camera: V4L2_CID_AUTOGAIN is not supported\n");
        } else {
                memset(&control, 0, sizeof (control));
                control.id = V4L2_CID_AUTOGAIN;
                control.value = 1;

                if (-1 == ioctl(camera->fd, VIDIOC_S_CTRL, &control)) {
                        log_err("Camera: VIDIOC_S_CTRL: error %d, %s", errno, strerror(errno));
                        return -1;
                }
        }
        */
 
       camera->state = CAMERA_INIT;

        return 0;
}

static int camera_close(camera_t* camera)
{
        if (camera->fd == -1)
                return 0;

        int oldfd = camera->fd;
        camera->fd = -1;
        return close(oldfd);
}

/*
static void print_palette(int p)
{
        switch (p) {
        case VIDEO_PALETTE_HI240:
                printf("High 240 cube (BT848)\n");
                break;

        case VIDEO_PALETTE_RGB565:
                printf("565 16 bit RGB\n");
                break;

        case VIDEO_PALETTE_RGB24:
                printf("24bit RGB\n");
                break;

        case VIDEO_PALETTE_RGB32:
                printf("32bit RGB\n");
                break;

        case VIDEO_PALETTE_RGB555:
                printf("555 15bit RGB\n");
                break;

        case VIDEO_PALETTE_YUV422:
                printf("YUV422 capture");
                break;

        case VIDEO_PALETTE_YUYV:
                printf("YUYV\n");
                break;

        case VIDEO_PALETTE_UYVY:
                printf("UYVY\n");
                break;

        case VIDEO_PALETTE_YUV420:
                printf("YUV420\n");
                break;

        case VIDEO_PALETTE_YUV411:
                printf("YUV411 capture\n");
                break;

        case VIDEO_PALETTE_RAW:
                printf("RAW capture (BT848)\n");
                break;

        case VIDEO_PALETTE_YUV422P:
                printf("YUV 4:2:2 Planar");
                break;

        case VIDEO_PALETTE_YUV411P:
                printf("YUV 4:1:1 Planar\n");
                break;

        case VIDEO_PALETTE_YUV420P:
                printf("YUV 4:2:0 Planar\n");
                break;

        case VIDEO_PALETTE_YUV410P:
                printf("YUV 4:1:0 Planar\n");
                break;
        }
}

static void camera_cap(camera_t* camera)
{
        char *msg;
        if (ioctl(camera->fd, VIDIOCGCAP, &camera->vid_cap) == -1) {
                if (camera->debug == TRUE) {
                        fprintf(stderr, "VIDIOCGCAP  --  could not get camera capabilities, exiting.....\n");
                }
                msg = g_strdup_printf(_("Could not connect to video device (%s).\nPlease check connection."), camera->video_dev);
                error_dialog(msg);
                g_free(msg);
                exit(0);
        }
        if (camera->x > 0 && camera->y > 0) {
                if(camera->vid_cap.maxwidth < camera->x) {
                        camera->x = camera->vid_cap.maxwidth;
                }
                if(camera->vid_cap.minwidth > camera->x) {
                        camera->x = camera->vid_cap.minwidth;
                }
                if(camera->vid_cap.maxheight < camera->y) {
                        camera->y = camera->vid_cap.maxheight;
                }
                if(camera->vid_cap.minheight > camera->y) {
                        camera->y = camera->vid_cap.minheight;
                }
        } else {
                switch (camera->size) {
                case PICMAX:
                        camera->x = camera->vid_cap.maxwidth;
                        camera->y = camera->vid_cap.maxheight;
                        break;

                case PICMIN:
                        camera->x = camera->vid_cap.minwidth;
                        camera->y = camera->vid_cap.minheight;
                        break;

                case PICHALF:
                        camera->x = camera->vid_cap.maxwidth / 2;
                        camera->y = camera->vid_cap.maxheight / 2;
                        break;

                default:
                        camera->x = camera->vid_cap.maxwidth / 2;
                        camera->y = camera->vid_cap.maxheight / 2;
                        break;
                }
        }
        if ((camera->vid_cap.type & VID_TYPE_CAPTURE) != 1) {
                camera->read = TRUE;
        }

        if (camera->debug == TRUE) {
                printf("\nVIDIOCGCAP\n");
                printf("device name = %s\n", camera->vid_cap.name);
                printf("device type = %d\n", camera->vid_cap.type);
                if(camera->read == FALSE){
                        printf("can use mmap()\n");
                }
                printf("# of channels = %d\n", camera->vid_cap.channels);
                printf("# of audio devices = %d\n", camera->vid_cap.audios);
                printf("max width = %d\n", camera->vid_cap.maxwidth);
                printf("max height = %d\n", camera->vid_cap.maxheight);
                printf("min width = %d\n", camera->vid_cap.minwidth);
                printf("min height = %d\n", camera->vid_cap.minheight);
        }
}

static void camera_get_pic_info(camera_t* camera)
{
        char *msg;
	
        if (ioctl(camera->fd, VIDIOCGPICT, &camera->vid_pic) == -1) {
                msg = g_strdup_printf(_("Could not connect to video device (%s).\nPlease check connection."), camera->video_dev);
                error_dialog(msg);
                if (camera->debug == TRUE) {
                        fprintf(stderr, "VIDIOCGPICT  --  could not get picture info, exiting....\n");
                }
                g_free(msg);
                exit(0);
        }
	
        if (camera->debug == TRUE) {
                printf("\nVIDIOCGPICT:\n");
                printf("bright = %d\n", camera->vid_pic.brightness);
                printf("hue = %d\n", camera->vid_pic.hue);
                printf("colour = %d\n", camera->vid_pic.colour);
                printf("contrast = %d\n", camera->vid_pic.contrast);
                printf("whiteness = %d\n", camera->vid_pic.whiteness);
                printf("colour depth = %d\n", camera->vid_pic.depth);
                print_palette(camera->vid_pic.palette);
        }
}

static void camera_get_win_info(camera_t* camera)
{
        gchar *msg;
        if (ioctl(camera->fd, VIDIOCGWIN, &camera->vid_win) == -1) {
                msg = g_strdup_printf(_("Could not connect to video device (%s).\nPlease check connection."), camera->video_dev);
                error_dialog(msg);
                if(camera->debug == TRUE) {
                        fprintf(stderr, "VIDIOCGWIN  --  could not get window info, exiting....\n");
                }
                exit(0);
        }
        if (camera->debug == TRUE) {
                printf("\nVIDIOCGWIN\n");
                printf("x = %d\n", camera->vid_win.x);
                printf("y = %d\n", camera->vid_win.y);
                printf("width = %d\n", camera->vid_win.width);
                printf("height = %d\n", camera->vid_win.height);
                printf("chromakey = %d\n", camera->vid_win.chromakey);
                printf("flags = %d\n", camera->vid_win.flags);
        }
}
*/
