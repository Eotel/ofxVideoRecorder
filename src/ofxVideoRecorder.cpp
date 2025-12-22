//
//  ofxVideoRecorder.cpp
//  ofxVideoRecorderExample
//
//  Created by Timothy Scaffidi on 9/23/12.
//  Copyright (c) 2012 __MyCompanyName__. All rights reserved.
//

#include "ofxVideoRecorder.h"

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <io.h>
#include <fcntl.h>
// Windows sleep replacement
#define usleep(x) Sleep((x)/1000)
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#endif

//--------------------------------------------------------------
//--------------------------------------------------------------
#ifndef _WIN32
int setNonBlocking(int fd){
#if defined(O_NONBLOCK)
    int flags;
    /* Fixme: O_NONBLOCK is defined but broken on SunOS 4.1.x and AIX 3.2.5. */
    if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
    /* Otherwise, use the old way of doing it */
    int flags = 1;
    return ioctl(fd, FIOBIO, &flags);
#endif
}
#endif

//--------------------------------------------------------------
//--------------------------------------------------------------
execThread::execThread(){
    execCommand = "";
    initialized = false;
}

//--------------------------------------------------------------
void execThread::setup(string command){
    execCommand = command;
    initialized = false;
    startThread(true);
}

//--------------------------------------------------------------
void execThread::threadedFunction(){
    if(isThreadRunning()){
        ofLogVerbose("execThread") << "starting command: " <<  execCommand;
        int result = system(execCommand.c_str());
        if (result == 0) {
            ofLogVerbose("execThread") << "command completed successfully.";
            initialized = true;
        } else {
            ofLogError("execThread") << "command failed with result: " << result;
        }
    }
}

//--------------------------------------------------------------
//--------------------------------------------------------------
ofxVideoDataWriterThread::ofxVideoDataWriterThread(){
#ifdef _WIN32
    hPipe = INVALID_HANDLE_VALUE;
#else
    fd = -1;
#endif
}

//--------------------------------------------------------------
void ofxVideoDataWriterThread::setup(string filePath, lockFreeQueue<ofPixels *> * q){
    this->filePath = filePath;
#ifdef _WIN32
    hPipe = INVALID_HANDLE_VALUE;
#else
    fd = -1;
#endif
    queue = q;
    bIsWriting = false;
    bClose = false;
    bNotifyError = false;
    startThread(true);
}

//--------------------------------------------------------------
void ofxVideoDataWriterThread::threadedFunction(){
#ifdef _WIN32
    if(hPipe == INVALID_HANDLE_VALUE){
        ofLogNotice("ofxVideoDataWriterThread") << "creating pipe: " <<  filePath;

        // Create the named pipe as server
        hPipe = CreateNamedPipeA(
            filePath.c_str(),
            PIPE_ACCESS_OUTBOUND,  // Write-only from our side
            PIPE_TYPE_BYTE | PIPE_WAIT,
            1,  // Max instances
            1024 * 1024,  // Output buffer size (1MB)
            0,  // Input buffer size
            0,  // Default timeout
            NULL  // Default security
        );

        if(hPipe == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            ofLogError("ofxVideoDataWriterThread") << "failed to create pipe, error: " << err;
            bNotifyError = true;
            return;
        }

        ofLogNotice("ofxVideoDataWriterThread") << "waiting for ffmpeg to connect...";

        // Wait for ffmpeg to connect to the pipe
        BOOL connected = ConnectNamedPipe(hPipe, NULL);
        if(!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
            DWORD err = GetLastError();
            ofLogError("ofxVideoDataWriterThread") << "ffmpeg failed to connect, error: " << err;
            CloseHandle(hPipe);
            hPipe = INVALID_HANDLE_VALUE;
            bNotifyError = true;
            return;
        }

        ofLogNotice("ofxVideoDataWriterThread") << "ffmpeg connected to pipe";
    }

    while(isThreadRunning())
    {
        ofPixels * frame = NULL;
        if(queue->Consume(frame) && frame){
            bIsWriting = true;
            DWORD b_offset = 0;
            DWORD b_remaining = (DWORD)(frame->getWidth()*frame->getHeight()*frame->getBytesPerPixel());

            while(b_remaining > 0 && isThreadRunning())
            {
                DWORD b_written = 0;
                BOOL success = WriteFile(hPipe, ((char *)frame->getData())+b_offset, b_remaining, &b_written, NULL);

                if(success && b_written > 0){
                    b_remaining -= b_written;
                    b_offset += b_written;
                    if (b_remaining != 0) {
                        ofLogWarning("ofxVideoDataWriterThread") << ofGetTimestampString("%H:%M:%S:%i") << " - b_remaining is not 0 -> " << b_written << " - " << b_remaining << " - " << b_offset << ".";
                    }
                }
                else if (!success) {
                    DWORD err = GetLastError();
                    ofLogError("ofxVideoDataWriterThread") << ofGetTimestampString("%H:%M:%S:%i") << " - write to PIPE failed with error -> " << err;
                    bNotifyError = true;
                    break;
                }
                else {
                    if(bClose){
                        ofLogVerbose("ofxVideoDataWriterThread") << ofGetTimestampString("%H:%M:%S:%i") << " - Nothing was written and bClose is TRUE.";
                        break;
                    }
                    ofLogWarning("ofxVideoDataWriterThread") << ofGetTimestampString("%H:%M:%S:%i") << " - Nothing was written. Is this normal?";
                }

                if (!isThreadRunning()) {
                    ofLogWarning("ofxVideoDataWriterThread") << ofGetTimestampString("%H:%M:%S:%i") << " - The thread is not running anymore let's get out of here!";
                }
            }
            bIsWriting = false;
            frame->clear();
            delete frame;
        }
        else{
            std::unique_lock<ofMutex> lock(conditionMutex);
            condition.wait(lock);
        }
    }

    ofLogVerbose("ofxVideoDataWriterThread") << "closing pipe: " <<  filePath;
    if(hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(hPipe);
        hPipe = INVALID_HANDLE_VALUE;
    }
#else
    if(fd == -1){
        ofLogVerbose("ofxVideoDataWriterThread") << "opening pipe: " <<  filePath;
        fd = ::open(filePath.c_str(), O_WRONLY);
        ofLogWarning("ofxVideoDataWriterThread") << "got file descriptor " << fd;
    }

    while(isThreadRunning())
    {
        ofPixels * frame = NULL;
        if(queue->Consume(frame) && frame){
            bIsWriting = true;
            int b_offset = 0;
            int b_remaining = frame->getWidth()*frame->getHeight()*frame->getBytesPerPixel();

            while(b_remaining > 0 && isThreadRunning())
            {
                errno = 0;

                int b_written = ::write(fd, ((char *)frame->getData())+b_offset, b_remaining);

                if(b_written > 0){
                    b_remaining -= b_written;
                    b_offset += b_written;
                    if (b_remaining != 0) {
                        ofLogWarning("ofxVideoDataWriterThread") << ofGetTimestampString("%H:%M:%S:%i") << " - b_remaining is not 0 -> " << b_written << " - " << b_remaining << " - " << b_offset << ".";
                        // break;
                    }
                }
                else if (b_written < 0) {
                    ofLogError("ofxVideoDataWriterThread") << ofGetTimestampString("%H:%M:%S:%i") << " - write to PIPE failed with error -> " << errno << " - " << strerror(errno) << ".";
                    bNotifyError = true;
                    break;
                }
                else {
                    if(bClose){
                        ofLogVerbose("ofxVideoDataWriterThread") << ofGetTimestampString("%H:%M:%S:%i") << " - Nothing was written and bClose is TRUE.";
                        break; // quit writing so we can close the file
                    }
                    ofLogWarning("ofxVideoDataWriterThread") << ofGetTimestampString("%H:%M:%S:%i") << " - Nothing was written. Is this normal?";
                }

                if (!isThreadRunning()) {
                    ofLogWarning("ofxVideoDataWriterThread") << ofGetTimestampString("%H:%M:%S:%i") << " - The thread is not running anymore let's get out of here!";
                }
            }
            bIsWriting = false;
            frame->clear();
            delete frame;
        }
        else{
            std::unique_lock<ofMutex> lock(conditionMutex);
            condition.wait(lock);
        }
    }

    ofLogVerbose("ofxVideoDataWriterThread") << "closing pipe: " <<  filePath;
    ::close(fd);
#endif
}

//--------------------------------------------------------------
void ofxVideoDataWriterThread::signal(){
    condition.notify_all();
}

//--------------------------------------------------------------
void ofxVideoDataWriterThread::setPipeNonBlocking(){
#ifndef _WIN32
    setNonBlocking(fd);
#endif
}

//--------------------------------------------------------------
//--------------------------------------------------------------
ofxAudioDataWriterThread::ofxAudioDataWriterThread(){
#ifdef _WIN32
    hPipe = INVALID_HANDLE_VALUE;
#else
    fd = -1;
#endif
}

//--------------------------------------------------------------
void ofxAudioDataWriterThread::setup(string filePath, lockFreeQueue<audioFrameShort *> *q){
    this->filePath = filePath;
#ifdef _WIN32
    hPipe = INVALID_HANDLE_VALUE;
#else
    fd = -1;
#endif
    queue = q;
    bIsWriting = false;
    bNotifyError = false;
    startThread(true);
}

//--------------------------------------------------------------
void ofxAudioDataWriterThread::threadedFunction(){
#ifdef _WIN32
    if(hPipe == INVALID_HANDLE_VALUE){
        ofLogNotice("ofxAudioDataWriterThread") << "creating pipe: " <<  filePath;

        // Create the named pipe as server
        hPipe = CreateNamedPipeA(
            filePath.c_str(),
            PIPE_ACCESS_OUTBOUND,  // Write-only from our side
            PIPE_TYPE_BYTE | PIPE_WAIT,
            1,  // Max instances
            1024 * 1024,  // Output buffer size (1MB)
            0,  // Input buffer size
            0,  // Default timeout
            NULL  // Default security
        );

        if(hPipe == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            ofLogError("ofxAudioDataWriterThread") << "failed to create pipe, error: " << err;
            bNotifyError = true;
            return;
        }

        ofLogNotice("ofxAudioDataWriterThread") << "waiting for ffmpeg to connect...";

        // Wait for ffmpeg to connect to the pipe
        BOOL connected = ConnectNamedPipe(hPipe, NULL);
        if(!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
            DWORD err = GetLastError();
            ofLogError("ofxAudioDataWriterThread") << "ffmpeg failed to connect, error: " << err;
            CloseHandle(hPipe);
            hPipe = INVALID_HANDLE_VALUE;
            bNotifyError = true;
            return;
        }

        ofLogNotice("ofxAudioDataWriterThread") << "ffmpeg connected to audio pipe";
    }

    while(isThreadRunning())
    {
        audioFrameShort * frame = NULL;
        if(queue->Consume(frame) && frame){
            bIsWriting = true;
            DWORD b_offset = 0;
            DWORD b_remaining = (DWORD)(frame->size*sizeof(short));
            while(b_remaining > 0 && isThreadRunning()){
                DWORD b_written = 0;
                BOOL success = WriteFile(hPipe, ((char *)frame->data)+b_offset, b_remaining, &b_written, NULL);

                if(success && b_written > 0){
                    b_remaining -= b_written;
                    b_offset += b_written;
                }
                else if (!success) {
                    DWORD err = GetLastError();
                    ofLogError("ofxAudioDataWriterThread") << ofGetTimestampString("%H:%M:%S:%i") << " - write to PIPE failed with error -> " << err;
                    bNotifyError = true;
                    break;
                }
                else {
                    if(bClose){
                        break;
                    }
                }

                if (!isThreadRunning()) {
                    ofLogWarning("ofxAudioDataWriterThread") << ofGetTimestampString("%H:%M:%S:%i") << " - The thread is not running anymore let's get out of here!";
                }
            }
            bIsWriting = false;
            delete [] frame->data;
            delete frame;
        }
        else{
            std::unique_lock<ofMutex> lock(conditionMutex);
            condition.wait(lock);
        }
    }

    ofLogVerbose("ofxAudioDataWriterThread") << "closing pipe: " <<  filePath;
    if(hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(hPipe);
        hPipe = INVALID_HANDLE_VALUE;
    }
#else
    if(fd == -1){
        ofLogVerbose("ofxAudioDataWriterThread") << "opening pipe: " <<  filePath;
        fd = ::open(filePath.c_str(), O_WRONLY);
        ofLogWarning("ofxAudioDataWriterThread") << "got file descriptor " << fd;
    }

    while(isThreadRunning())
    {
        audioFrameShort * frame = NULL;
        if(queue->Consume(frame) && frame){
            bIsWriting = true;
            int b_offset = 0;
            int b_remaining = frame->size*sizeof(short);
            while(b_remaining > 0 && isThreadRunning()){
                int b_written = ::write(fd, ((char *)frame->data)+b_offset, b_remaining);

                if(b_written > 0){
                    b_remaining -= b_written;
                    b_offset += b_written;
                }
                else if (b_written < 0) {
                    ofLogError("ofxAudioDataWriterThread") << ofGetTimestampString("%H:%M:%S:%i") << " - write to PIPE failed with error -> " << errno << " - " << strerror(errno) << ".";
                    bNotifyError = true;
                    break;
                }
                else {
                    if(bClose){
                        // quit writing so we can close the file
                        break;
                    }
                }

                if (!isThreadRunning()) {
                    ofLogWarning("ofxAudioDataWriterThread") << ofGetTimestampString("%H:%M:%S:%i") << " - The thread is not running anymore let's get out of here!";
                }
            }
            bIsWriting = false;
            delete [] frame->data;
            delete frame;
        }
        else{
            std::unique_lock<ofMutex> lock(conditionMutex);
            condition.wait(lock);
        }
    }

    ofLogVerbose("ofxAudioDataWriterThread") << "closing pipe: " <<  filePath;
    ::close(fd);
#endif
}

//--------------------------------------------------------------
void ofxAudioDataWriterThread::signal(){
    condition.notify_all();
}

//--------------------------------------------------------------
void ofxAudioDataWriterThread::setPipeNonBlocking(){
#ifndef _WIN32
    setNonBlocking(fd);
#endif
}

//--------------------------------------------------------------
//--------------------------------------------------------------
ofxVideoRecorder::ofxVideoRecorder(){
    bIsInitialized = false;
    ffmpegLocation = "ffmpeg";
    videoCodec = "mpeg4";
    audioCodec = "pcm_s16le";
    videoBitrate = "2000k";
    audioBitrate = "128k";
    pixelFormat = "rgb24";
    outputPixelFormat = "";
}

//--------------------------------------------------------------
bool ofxVideoRecorder::setup(string fname, int w, int h, float fps, int sampleRate, int channels, bool sysClockSync, bool silent){
    if(bIsInitialized)
    {
        close();
    }

    fileName = fname;
    string absFilePath = ofFilePath::getAbsolutePath(fileName);

    moviePath = ofFilePath::getAbsolutePath(fileName);

    stringstream outputSettings;
    outputSettings
    << " -vcodec " << videoCodec
    << " -b " << videoBitrate
    << " -acodec " << audioCodec
    << " -ab " << audioBitrate
    << " \"" << absFilePath << "\"";

    return setupCustomOutput(w, h, fps, sampleRate, channels, outputSettings.str(), sysClockSync, silent);
}

//--------------------------------------------------------------
bool ofxVideoRecorder::setupCustomOutput(int w, int h, float fps, string outputString, bool sysClockSync, bool silent){
    return setupCustomOutput(w, h, fps, 0, 0, outputString, sysClockSync, silent);
}

//--------------------------------------------------------------
bool ofxVideoRecorder::setupCustomOutput(int w, int h, float fps, int sampleRate, int channels, string outputString, bool sysClockSync, bool silent){
    if(bIsInitialized)
    {
        close();
    }

    bIsSilent = silent;
    bSysClockSync = sysClockSync;

    bRecordAudio = (sampleRate > 0 && channels > 0);
    bRecordVideo = (w > 0 && h > 0 && fps > 0);
    bFinishing = false;

    videoFramesRecorded = 0;
    audioSamplesRecorded = 0;

    if(!bRecordVideo && !bRecordAudio) {
        ofLogWarning() << "ofxVideoRecorder::setupCustomOutput(): invalid parameters, could not setup video or audio stream.\n"
        << "video: " << w << "x" << h << "@" << fps << "fps\n"
        << "audio: " << "channels: " << channels << " @ " << sampleRate << "Hz\n";
        return false;
    }
    videoPipePath = "";
    audioPipePath = "";
    pipeNumber = requestPipeNumber();

#ifdef _WIN32
    // Windows: Use named pipes with \\.\pipe\pipename format
    if(bRecordVideo) {
        width = w;
        height = h;
        frameRate = fps;

        // Create Windows named pipe path
        videoPipePath = "\\\\.\\pipe\\ofxvrpipe" + ofToString(pipeNumber);
        ofLogNotice("ofxVideoRecorder") << "Creating video pipe: " << videoPipePath;
    }

    if(bRecordAudio) {
        this->sampleRate = sampleRate;
        audioChannels = channels;

        // Create Windows named pipe path
        audioPipePath = "\\\\.\\pipe\\ofxarpipe" + ofToString(pipeNumber);
        ofLogNotice("ofxVideoRecorder") << "Creating audio pipe: " << audioPipePath;
    }

    stringstream cmd;
    // Windows: Run ffmpeg directly without bash wrapper
    // Use "start /B" for background execution
    cmd << "start /B \"\" " << ffmpegLocation << (bIsSilent?" -loglevel quiet ":" ") << "-y";
    if(bRecordAudio){
        cmd << " -acodec pcm_s16le -f s16le -ar " << sampleRate << " -ac " << audioChannels << " -i \"" << audioPipePath << "\"";
    }
    else {
        cmd << " -an";
    }
    if(bRecordVideo){
        cmd << " -r "<< fps << " -s " << w << "x" << h << " -f rawvideo -pix_fmt " << pixelFormat <<" -i \"" << videoPipePath << "\" -r " << fps;
        if (outputPixelFormat.length() > 0)
            cmd << " -pix_fmt " << outputPixelFormat;
    }
    else {
        cmd << " -vn";
    }
    cmd << " " + outputString;

#else
    // Unix: Use FIFO pipes with mkfifo
    if(bRecordVideo) {
        width = w;
        height = h;
        frameRate = fps;

        // recording video, create a FIFO pipe
        videoPipePath = ofFilePath::getAbsolutePath("ofxvrpipe" + ofToString(pipeNumber));
        if(!ofFile::doesFileExist(videoPipePath)){
            string mkfifoCmd = "bash --login -c 'mkfifo " + videoPipePath + "'";
            system(mkfifoCmd.c_str());
        }
    }

    if(bRecordAudio) {
        this->sampleRate = sampleRate;
        audioChannels = channels;

        // recording audio, create a FIFO pipe
        audioPipePath = ofFilePath::getAbsolutePath("ofxarpipe" + ofToString(pipeNumber));
        if(!ofFile::doesFileExist(audioPipePath)){
            string mkfifoCmd = "bash --login -c 'mkfifo " + audioPipePath + "'";
            system(mkfifoCmd.c_str());
        }
    }

    stringstream cmd;
    // Unix: basic ffmpeg invocation, -y option overwrites output file
    cmd << "bash --login -c '" << ffmpegLocation << (bIsSilent?" -loglevel quiet ":" ") << "-y";
    if(bRecordAudio){
        cmd << " -acodec pcm_s16le -f s16le -ar " << sampleRate << " -ac " << audioChannels << " -i \"" << audioPipePath << "\"";
    }
    else {
        cmd << " -an";
    }
    if(bRecordVideo){
        cmd << " -r "<< fps << " -s " << w << "x" << h << " -f rawvideo -pix_fmt " << pixelFormat <<" -i \"" << videoPipePath << "\" -r " << fps;
        if (outputPixelFormat.length() > 0)
            cmd << " -pix_fmt " << outputPixelFormat;
    }
    else {
        cmd << " -vn";
    }
    cmd << " "+ outputString +"' &";
#endif

    ofLogNotice("ofxVideoRecorder") << "FFmpeg command: " << cmd.str();

#ifdef _WIN32
    // Windows: Start pipe writer threads FIRST to create the named pipes,
    // then start ffmpeg which will connect to them
    if(bRecordAudio){
        audioThread.setup(audioPipePath, &audioFrames);
    }
    if(bRecordVideo){
        videoThread.setup(videoPipePath, &frames);
    }

    // Give the threads time to create the pipes before starting ffmpeg
    Sleep(100);

    // start ffmpeg thread. FFmpeg will connect to the named pipes.
    ffmpegThread.setup(cmd.str());

    // wait until ffmpeg has started
    while (!ffmpegThread.isInitialized()) {
        Sleep(10);
    }
#else
    // Unix: start ffmpeg thread. Ffmpeg will wait for input pipes to be opened.
    ffmpegThread.setup(cmd.str());

    // wait until ffmpeg has started
    while (!ffmpegThread.isInitialized()) {
        usleep(10);
    }

    if(bRecordAudio){
        audioThread.setup(audioPipePath, &audioFrames);
    }
    if(bRecordVideo){
        videoThread.setup(videoPipePath, &frames);
    }
#endif

    bIsInitialized = true;
    bIsRecording = false;
    bIsPaused = false;

    startTime = 0;
    recordingDuration = 0;
    totalRecordingDuration = 0;

    return bIsInitialized;
}

//--------------------------------------------------------------
bool ofxVideoRecorder::addFrame(const ofPixels &pixels){
    if (!bIsRecording || bIsPaused) return false;

    if(bIsInitialized && bRecordVideo && ffmpegThread.isInitialized())
    {
        int framesToAdd = 1; // default add one frame per request

        if((bRecordAudio || bSysClockSync) && !bFinishing){

            double syncDelta;
            double videoRecordedTime = videoFramesRecorded / frameRate;

            if (bRecordAudio) {
                // if also recording audio, check the overall recorded time for audio and video to make sure audio is not going out of sync
                // this also handles incoming dynamic framerate while maintaining desired outgoing framerate
                double audioRecordedTime = (audioSamplesRecorded/audioChannels)  / (double)sampleRate;
                syncDelta = audioRecordedTime - videoRecordedTime;
            }
            else {
                // if just recording video, synchronize the video against the system clock
                // this also handles incoming dynamic framerate while maintaining desired outgoing framerate
                syncDelta = systemClock() - videoRecordedTime;
            }

            if(syncDelta > 1.0/frameRate) {
                // not enought video frames, we need to send extra video frames.
                while(syncDelta > 1.0/frameRate) {
                    framesToAdd++;
                    syncDelta -= 1.0/frameRate;
                }
                ofLogVerbose() << "ofxVideoRecorder: recDelta = " << syncDelta << ". Not enough video frames for desired frame rate, copied this frame " << framesToAdd << " times.\n";
            }
            else if(syncDelta < -1.0/frameRate){
                // more than one video frame is waiting, skip this frame
                framesToAdd = 0;
                ofLogVerbose() << "ofxVideoRecorder: recDelta = " << syncDelta << ". Too many video frames, skipping.\n";
            }
        }

        for(int i=0;i<framesToAdd;i++){
            // add desired number of frames
            frames.Produce(new ofPixels(pixels));
            videoFramesRecorded++;
        }

        videoThread.signal();

        return true;
    }

    return false;
}

//--------------------------------------------------------------
void ofxVideoRecorder::addAudioSamples(float *samples, int bufferSize, int numChannels){
    if (!bIsRecording || bIsPaused) return;

    if(bIsInitialized && bRecordAudio){
        int size = bufferSize*numChannels;
        audioFrameShort * shortSamples = new audioFrameShort;
        shortSamples->data = new short[size];
        shortSamples->size = size;

        for(int i=0; i < size; i++){
            shortSamples->data[i] = (short)(samples[i] * 32767.0f);
        }
        audioFrames.Produce(shortSamples);
        audioThread.signal();
        audioSamplesRecorded += size;
    }
}

//--------------------------------------------------------------
void ofxVideoRecorder::start(){
    if(!bIsInitialized) return;

    if (bIsRecording) {
        // We are already recording. No need to go further.
       return;
    }

    // Start a recording.
    bIsRecording = true;
    bIsPaused = false;
    startTime = ofGetElapsedTimef();

    ofLogVerbose() << "Recording." << endl;
}

//--------------------------------------------------------------
void ofxVideoRecorder::setPaused(bool bPause){
    if(!bIsInitialized) return;

    if (!bIsRecording || bIsPaused == bPause) {
        //  We are not recording or we are already paused. No need to go further.
        return;
    }

    // Pause the recording
    bIsPaused = bPause;

    if (bIsPaused) {
        totalRecordingDuration += recordingDuration;

        // Log
        ofLogVerbose() << "Paused." << endl;
    } else {
        startTime = ofGetElapsedTimef();

        // Log
        ofLogVerbose() << "Recording." << endl;
    }
}

//--------------------------------------------------------------
void ofxVideoRecorder::close(){
    if(!bIsInitialized) return;

    bIsRecording = false;

    if(bRecordVideo && bRecordAudio) {
        // set pipes to non_blocking so we dont get stuck at the final writes
        // audioThread.setPipeNonBlocking();
        // videoThread.setPipeNonBlocking();

        if (frames.size() > 0 && audioFrames.size() > 0) {
            // if there are frames in the queue start a thread to finalize the output file without blocking the app.
            startThread();
            return;
        }
    }
    else if(bRecordVideo) {
        // set pipes to non_blocking so we dont get stuck at the final writes
        // videoThread.setPipeNonBlocking();

        if (frames.size() > 0) {
            // if there are frames in the queue start a thread to finalize the output file without blocking the app.
            startThread();
            return;
        }
        else {
            // cout << "ofxVideoRecorder :: we are good to go!" << endl;
        }

    }
    else if(bRecordAudio) {
        // set pipes to non_blocking so we dont get stuck at the final writes
        // audioThread.setPipeNonBlocking();

        if (audioFrames.size() > 0) {
            // if there are frames in the queue start a thread to finalize the output file without blocking the app.
            startThread();
            return;
        }
    }

    outputFileComplete();
}

//--------------------------------------------------------------
void ofxVideoRecorder::threadedFunction()
{
    if(bRecordVideo && bRecordAudio) {
        while(frames.size() > 0 && audioFrames.size() > 0) {
            // if there are frames in the queue or the thread is writing, signal them until the work is done.
            videoThread.signal();
            audioThread.signal();
        }
    }
    else if(bRecordVideo) {
        while(frames.size() > 0) {
            // if there are frames in the queue or the thread is writing, signal them until the work is done.
            videoThread.signal();
        }
    }
    else if(bRecordAudio) {
        while(audioFrames.size() > 0) {
            // if there are frames in the queue or the thread is writing, signal them until the work is done.
            audioThread.signal();
        }
    }

    waitForThread();

    outputFileComplete();
}

//--------------------------------------------------------------
void ofxVideoRecorder::outputFileComplete()
{
    // at this point all data that ffmpeg wants should have been consumed
    // one of the threads may still be trying to write a frame,
    // but once close() gets called they will exit the non_blocking write loop
    // and hopefully close successfully

    bIsInitialized = false;

    if (bRecordVideo) {
        videoThread.close();
    }
    if (bRecordAudio) {
        audioThread.close();
    }

    retirePipeNumber(pipeNumber);

    ffmpegThread.waitForThread();
    // TODO: kill ffmpeg process if its taking too long to close for whatever reason.

    // Notify the listeners.
    ofxVideoRecorderOutputFileCompleteEventArgs args;
    args.fileName = fileName;
    ofNotifyEvent(outputFileCompleteEvent, args);
}

//--------------------------------------------------------------
bool ofxVideoRecorder::hasVideoError(){
    return videoThread.bNotifyError;
}

//--------------------------------------------------------------
bool ofxVideoRecorder::hasAudioError(){
    return audioThread.bNotifyError;
}

//--------------------------------------------------------------
float ofxVideoRecorder::systemClock(){
    recordingDuration = ofGetElapsedTimef() - startTime;
    return totalRecordingDuration + recordingDuration;
}

//--------------------------------------------------------------
std::set<int> ofxVideoRecorder::openPipes;

//--------------------------------------------------------------
int ofxVideoRecorder::requestPipeNumber(){
    int n = 0;
    while (openPipes.find(n) != openPipes.end()) {
        n++;
    }
    openPipes.insert(n);
    return n;
}

//--------------------------------------------------------------
void ofxVideoRecorder::retirePipeNumber(int num){
    if(!openPipes.erase(num)){
        ofLogNotice() << "ofxVideoRecorder::retirePipeNumber(): trying to retire a pipe number that is not being tracked: " << num << endl;
    }
}
