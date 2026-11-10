/*
    video.h - playing .mpg videos (MPEG-1 video with MP2 sound), full screen

    PLY plays a video when the file is one (and a WAV or MP3 when it isn't), and AerLang has
    video(path). While it plays:

        Esc or Ctrl+C   stop
        Space           pause / carry on
        Left / Right    5 seconds back / on
        any other key   show the time and the controls for a few seconds

    The picture is made as big as fits the screen, keeping its shape, and the sound plays in
    step with it. How it all works is at the top of video.c; how to make .mpg files is in
    lib/mpg.h and docs/VIDEO.md.
*/
#ifndef VIDEO_H
#define VIDEO_H

#include <stdbool.h>

#define VIDEO_OK              0
#define VIDEO_ERR_NOT_FOUND  -1
#define VIDEO_ERR_FORMAT     -2  // not an .mpg with MPEG-1 video in it
#define VIDEO_ERR_NO_SCREEN  -3  // VGA text mode: there are no pixels to draw on
#define VIDEO_ERR_NO_MEMORY  -4

// does this file look like a video? (it starts the way every .mpg file does)
bool video_is_video (const char* path);

// play a video full screen until it ends or the user stops it, then give the screen back
int video_play_file (const char* path);

// a short description of a VIDEO_ result, for messages
const char* video_strerror (int error);

#endif /* VIDEO_H */
