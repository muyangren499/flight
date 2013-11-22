/**
 * Program that grabs stereo pairs from Point Grey
 * Firefly MV USB cameras, perfroms the single-disparity
 * stereo algorithm on them, and publishes the results
 * to LCM.
 *
 * Written by Andrew Barry <abarry@csail.mit.edu>, 2013
 *
 */

#include "opencv-stereo.hpp"



bool show_display; // set to true to show opencv images on screen
bool enable_online_recording = false;  // set to true to enable online recording
bool disable_stereo = false;
bool show_unrectified = false;

int force_brightness = -1;
int force_exposure = -1;

// allocate a huge array for a ringbuffer
Mat ringbufferL[RINGBUFFER_SIZE];
Mat ringbufferR[RINGBUFFER_SIZE];


// global for where we are drawing a line on the image
int lineLeftImgPosition = -1;
int lineLeftImgPositionY = -1;

// lcm subscription to the control channel
lcmt_stereo_control_subscription_t *stereo_control_sub;


int numFrames = 0;
int recNumFrames = 0;
int video_number = -1;
bool recordingOn = true;
bool using_video_file = false;

dc1394_t        *d;
dc1394camera_t  *camera;

dc1394_t        *d2;
dc1394camera_t  *camera2;

OpenCvStereoConfig stereoConfig;

/**
 * Cleanly handles and exit from a command-line
 * ctrl-c signal.
 *
 * @param s
 *
 */
void control_c_handler(int s)
{
    cout << endl << "exiting via ctrl-c" << endl;
  
    StopCapture(d, camera);
    StopCapture(d2, camera2);
    
    if (enable_online_recording == false)
    {
        cout << "\tpress ctrl+\\ to quit while writing video." << endl;
        WriteVideo();
    }
    
    exit(0);
}



void lcm_stereo_control_handler(const lcm_recv_buf_t *rbuf, const char* channel, const lcmt_stereo_control *msg, void *user)
{
    /*
     * The way this works is that as soon as
     * the program starts, stereo is always running.
     *
     * stereo control:
     *   0: write to disk and then restart
     *   1: (re)start record
     *   2: pause
     * 
     */
     
    // got a control message, so parse it and figure out what we should do
    if (msg->stereo_control == 0)
    {
        // write video, then start recording again
        
        if (enable_online_recording == false)
        {
            WriteVideo();
        }
        
        StartRecording();
        
    } else if (msg->stereo_control == 1)
    {
        // record
        cout << "(Re)starting recording." << endl;
        
        StartRecording();
        
    } else if (msg->stereo_control == 2)
    {
        // pause recording if we were recording
        cout << "Recording paused." << endl;
        
        recordingOn = false;
    } else {
        // got an unknown command
        fprintf(stderr, "WARNING: Unknown stereo_control command: %d", int(msg->stereo_control));
    }
    
}

void StartRecording()
{
    // get a new filename
    video_number = GetNextVideoNumber(stereoConfig);
    
    recordingOn = true;
    recNumFrames = 0;
}


void NonBlockingLcm(lcm_t *lcm)
{
    // setup an lcm function that won't block when we read it
    int lcm_fd = lcm_get_fileno(lcm);
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(lcm_fd, &fds);
    
    // wait a limited amount of time for an incoming message
    struct timeval timeout = {
        0,  // seconds
        1   // microseconds
    };

    
    int status = select(lcm_fd + 1, &fds, 0, 0, &timeout);

    if(0 == status) {
        // no messages
        //do nothing
        
    } else if(FD_ISSET(lcm_fd, &fds)) {
        // LCM has events ready to be processed.
        lcm_handle(lcm);
    }

}

int main(int argc, char *argv[])
{
    // get input arguments
    
    string configFile = "";
    string video_file_left = "", video_file_right = "";
    
    ConciseArgs parser(argc, argv);
    parser.add(configFile, "c", "config", "Configuration file containing camera GUIDs, etc.", true);
    parser.add(show_display, "d", "show-dispaly", "Enable for visual debugging display. Will reduce framerate significantly.");
    parser.add(show_unrectified, "u", "show-unrectified", "When displaying images, do not apply rectification.");
    parser.add(enable_online_recording, "r", "online-record", "Enable online video recording.");
    parser.add(disable_stereo, "s", "disable-stereo", "Disable online stereo processing.");
    parser.add(force_brightness, "b", "force-brightness", "Force a brightness setting.");
    parser.add(force_exposure, "e", "force-exposure", "Force an exposure setting.");
    parser.add(video_file_left, "l", "video-file-left", "Do not use cameras, instead use this video file (also requires a right video file).");
    parser.add(video_file_right, "t", "video-file-right", "Right video file, only for use with the -l option.");
    parser.parse();
    
    // parse the config file
    if (ParseConfigFile(configFile, &stereoConfig) != true)
    {
        fprintf(stderr, "Failed to parse configuration file, quitting.\n");
        return -1;
    }
    
    if (video_file_left.length() > 0
        && video_file_right.length() <= 0) {
        
        fprintf(stderr, "Error: for playback you must specify both "
            "a right and left video file. (Only got a left one.)\n");

        return -1;
    }
    
     if (video_file_left.length() <= 0
        && video_file_right.length() > 0) {
        
        fprintf(stderr, "Error: for playback you must specify both "
            "a right and left video file. (Only got a left one.)\n");
        
        return -1;
    }
    
    if (video_file_left.length() > 0) {
        using_video_file = true;
    }
    
    if (using_video_file && enable_online_recording)
    {
        cout << "Cannot record while using a video file. Ignoring -r"
            << endl;
            
        enable_online_recording = false;
    }
    
    uint64 guid = stereoConfig.guidLeft;
    uint64 guid2 = stereoConfig.guidRight;
    
    // start up LCM
    lcm_t * lcm;
    lcm = lcm_create (stereoConfig.lcmUrl.c_str());
    
    
    unsigned long elapsed;
    
    
    
    
    // --- setup control-c handling ---
    struct sigaction sigIntHandler;

    sigIntHandler.sa_handler = control_c_handler;
    sigemptyset(&sigIntHandler.sa_mask);
    sigIntHandler.sa_flags = 0;

    sigaction(SIGINT, &sigIntHandler, NULL);
    // --- end ctrl-c handling code ---
    
    dc1394error_t   err;
    dc1394error_t   err2;
    
    VideoCapture left_video_capture;
    VideoCapture right_video_capture;
    
    
    
    // tell opencv to use only one core so that we can manage our
    // own threading without a fight
    setNumThreads(1);
    
    if (!using_video_file) {
        d = dc1394_new ();
        if (!d)
            cerr << "Could not create dc1394 context" << endl;
            
        d2 = dc1394_new ();
        if (!d2)
            cerr << "Could not create dc1394 context for camera 2" << endl;

        camera = dc1394_camera_new (d, guid);
        if (!camera)
        {
            cerr << "Could not create dc1394 camera... quitting." << endl;
            exit(1);
        }
        
        camera2 = dc1394_camera_new (d2, guid2);
        if (!camera2)
            cerr << "Could not create dc1394 camera for camera 2" << endl;
        // reset the bus
        dc1394_reset_bus(camera);
        dc1394_reset_bus(camera2);
        
        // setup
        err = setup_gray_capture(camera, DC1394_VIDEO_MODE_FORMAT7_1);
        DC1394_ERR_CLN_RTN(err, cleanup_and_exit(camera), "Could not setup camera");

        err2 = setup_gray_capture(camera2, DC1394_VIDEO_MODE_FORMAT7_1);
        DC1394_ERR_CLN_RTN(err2, cleanup_and_exit(camera2), "Could not setup camera number 2");
        
        // enable camera
        err = dc1394_video_set_transmission(camera, DC1394_ON);
        DC1394_ERR_CLN_RTN(err, cleanup_and_exit(camera), "Could not start camera iso transmission");
        err2 = dc1394_video_set_transmission(camera2, DC1394_ON);
        DC1394_ERR_CLN_RTN(err2, cleanup_and_exit(camera2), "Could not start camera iso transmission for camera number 2");
        
        InitBrightnessSettings(camera, camera2);
    } else { // !using_video_file
        // we are using a video file, setup the readers
        
        if (left_video_capture.open(video_file_left) != true) {
            cerr << "Error: failed to open " << video_file_left
                << endl;
            return -1;
        } else {
            cout << "Opened " << video_file_left << endl;
        }
        
        if (right_video_capture.open(video_file_right) != true) {
            cerr << "Error: failed to open " << video_file_right
                << endl;
            return -1;
        } else {
            cout << "Opened " << video_file_right << endl;
        }
        
    }
	
    if (show_display)
    {
        namedWindow("Input", CV_WINDOW_AUTOSIZE);
        namedWindow("Input2", CV_WINDOW_AUTOSIZE);
        namedWindow("Stereo", CV_WINDOW_AUTOSIZE);
        
        setMouseCallback("Input", onMouse); // for drawing disparity lines
        setMouseCallback("Stereo", onMouse); // for drawing disparity lines
        
        cvMoveWindow("Input", 100, 100);
        cvMoveWindow("Stereo", 100, 370);
        cvMoveWindow("Input2", 500, 100);
    }
    // load calibration
    OpenCvStereoCalibration stereoCalibration;
    
    if (LoadCalibration(stereoConfig.calibrationDir, &stereoCalibration) != true)
    {
        cerr << "Error: failed to read calibration files. Quitting." << endl;
        return -1;
    }
    
    // subscribe to the stereo control channel
    stereo_control_sub = lcmt_stereo_control_subscribe(lcm, stereoConfig.stereoControlChannel.c_str(), &lcm_stereo_control_handler, NULL);
    
    Mat imgDisp;
    Mat imgDisp2;
    
    // initilize default parameters
    BarryMooreState state;
    
    state.disparity = stereoConfig.disparity;
    state.sobelLimit = stereoConfig.interestOperatorLimit;
    state.blockSize = stereoConfig.blockSize;
    
    if (state.blockSize > 10 || state.blockSize < 1)
    {
        fprintf(stderr, "Warning: block size is very large "
            "or small (%d).  Expect trouble.\n", state.blockSize);
    }
    
    state.sadThreshold = stereoConfig.sadThreshold;
    state.sobelAdd = stereoConfig.interestOperatorDivisor;
    
    state.mapxL = stereoCalibration.mx1fp;
    state.mapxR = stereoCalibration.mx2fp;
    state.Q = stereoCalibration.qMat;
    state.show_display = show_display;
    
    Mat matL, matR;
    bool quit = false;
    
    VideoWriter recordOnlyL, recordOnlyR;
    
    if (!using_video_file) {
        // allocate a huge buffer for video frames
        printf("Allocating ringbuffer data...\n");
        matL = GetFrameFormat7(camera);
        matR = GetFrameFormat7(camera2);
        for (int i=0; i<RINGBUFFER_SIZE; i++)
        {
            ringbufferL[i].create(matL.size(), matL.type());
            ringbufferR[i].create(matR.size(), matR.type());
        }
        printf("done.\n");
        
        StartRecording(); // set up recording variables
        
        if (enable_online_recording == true)
        {
            // setup video writers
            recordOnlyL = SetupVideoWriter("videoL-online", matL.size(), stereoConfig);
            recordOnlyR = SetupVideoWriter("videoR-online", matR.size(), stereoConfig, false);
        }
        
        // before we start, turn the cameras on and set the brightness and exposure
        MatchBrightnessSettings(camera, camera2, true, force_brightness, force_exposure);

    } // using_video_file
    
    // start the framerate clock
    struct timeval start, now;
    gettimeofday( &start, NULL );
    
    while (quit == false) {
    
        // get the frames from the camera
        if (!using_video_file) {
            // we would like to match brightness every frame
            // but that would really hurt our framerate
            // match brightness every 10 frames instead
            if (numFrames % MATCH_BRIGHTNESS_EVERY_N_FRAMES == 0)
            {
                MatchBrightnessSettings(camera, camera2);
            }
        
            // capture images from the cameras
            matL = GetFrameFormat7(camera);
            matR = GetFrameFormat7(camera2);
            
            // record video
            if (recordingOn == true)
            {
                ringbufferL[recNumFrames%RINGBUFFER_SIZE] = matL;
                ringbufferR[recNumFrames%RINGBUFFER_SIZE] = matR;
                
                recNumFrames ++;
            }
        } else {
            // using a video file -- get the next frame
            
            Mat matL_file, matR_file;
            
            left_video_capture >> matL_file;
            right_video_capture >> matR_file;
            
            // convert to CV_8UC1
            matL_file.convertTo(matL, CV_8UC1);
            matR_file.convertTo(matR, CV_8UC1);
            
        }
        
        
        Mat matDisp, remapL, remapR;
        
        if (show_display)
        {
            // we remap again here because we're just in display
            Mat remapLtemp(matL.rows, matL.cols, matL.depth());
            Mat remapRtemp(matR.rows, matR.cols, matR.depth());
            
            remapL = remapLtemp;
            remapR = remapRtemp;
            
            remap(matL, remapL, stereoCalibration.mx1fp, Mat(), INTER_NEAREST);
            remap(matR, remapR, stereoCalibration.mx2fp, Mat(), INTER_NEAREST);
            
            remapL.copyTo(matDisp);
        } // end show_display
        
        cv::vector<Point3f> pointVector3d;
        cv::vector<uchar> pointColors;
        cv::vector<Point3i> pointVector2d; // for display
        
        // do the main stereo processing
        if (disable_stereo != true)
        {
            StereoBarryMoore(matL, matR, &pointVector3d, &pointColors, &pointVector2d, state);
        }
            
        if (enable_online_recording == true)
        {
            // record frames
            recordOnlyL << matL;
            recordOnlyR << matR;
        }
            
        // build an LCM message for the stereo data
        lcmt_stereo msg;
        msg.timestamp = getTimestampNow();
        msg.number_of_points = (int)pointVector3d.size();
        
        float x[msg.number_of_points];
        float y[msg.number_of_points];
        float z[msg.number_of_points];
        uchar grey[msg.number_of_points];
        
        for (unsigned int i=0;i<pointVector3d.size();i++)
        {
            x[i] = pointVector3d[i].x;
            y[i] = pointVector3d[i].y;
            z[i] = pointVector3d[i].z;
            grey[i] = pointColors[i];
        }
        
        msg.x = x;
        msg.y = y;
        msg.z = z;
        msg.grey = grey;
        msg.frame_number = recNumFrames;
        msg.video_number = video_number;
        
        // publish the LCM message
        lcmt_stereo_publish(lcm, "stereo", &msg);
        
        
        if (show_display)
        {
//            matDisp = Mat::zeros(matL.rows, matL.cols, CV_8UC1);
            for (unsigned int i=0;i<pointVector2d.size();i++)
            {
                int x2 = pointVector2d[i].x;
                int y2 = pointVector2d[i].y;
                int sad = pointVector2d[i].z;
                rectangle(matDisp, Point(x2,y2), Point(x2+state.blockSize, y2+state.blockSize), sad,  CV_FILLED);
                rectangle(matDisp, Point(x2+1,y2+1), Point(x2+state.blockSize-1, y2-1+state.blockSize), 255);
            }
            
            // draw a line for the user to show disparity
            DrawLines(remapL, remapR, matDisp, lineLeftImgPosition, lineLeftImgPositionY, state.disparity);
            
            // OMG WHAT A HACK
            //Laplacian(matL, matDisp, -1);
            //Laplacian(matL, matDisp, -1, 3);
            //Sobel(matL, remapR, -1, 1, 1, 3, 2, 0);
            
            
            if (show_unrectified == false)
            {
                imshow("Input", remapL);
                imshow("Input2", remapR);
            } else {
                imshow("Input", matL);
                imshow("Input2", matR);
            }
            
            imshow("Stereo", matDisp);
                
            char key = waitKey(1);
            
            if (key != 255)
            {
                cout << endl << key << endl;
            }
            
            switch (key)
            {
                case 'T':
                    state.disparity --;
                    break;
                case 'R':
                    state.disparity ++;
                    break;
                    
                case 'w':
                    state.sobelLimit += 10;
                    break;
                    
                case 's':
                    state.sobelLimit -= 10;
                    break;
                    
                case 'g':
                    state.blockSize ++;
                    break;
                    
                case 'b':
                    state.blockSize --;
                    break;
                    
                case 'y':
                    state.sadThreshold ++;
                    break;
                    
                case 'h':
                    state.sadThreshold --;
                    break;
                    
                case 'u':
                    state.sobelAdd += 0.2;
                    break;
                    
                case 'j':
                    state.sobelAdd -= 0.2;
                    break;
                    
                case 'm':
                    MatchBrightnessSettings(camera, camera2, true, force_brightness, force_exposure);
                    break;
                    
                case '1':
                    force_brightness --;
                    MatchBrightnessSettings(camera, camera2, true, force_brightness, force_exposure);
                    break;
                    
                case '2':
                    force_brightness ++;
                    MatchBrightnessSettings(camera, camera2, true, force_brightness, force_exposure);
                    break;
                    
                case '3':
                    force_exposure --;
                    MatchBrightnessSettings(camera, camera2, true, force_brightness, force_exposure);
                    break;
                    
                case '4':
                    force_exposure ++;
                    MatchBrightnessSettings(camera, camera2, true, force_brightness, force_exposure);
                    break;
                    
                case '5':
                    // to show SAD boxes
                    state.sobelLimit = 0;
                    state.sadThreshold = 255;
                    break;
                    
                case 'q':
                    quit = true;
                    break;
            }
            
            if (key != 255)
            {
                cout << "brightness: " << force_brightness << endl;
                cout << "exposure: " << force_exposure << endl;
                cout << "disparity = " << state.disparity << endl;
                cout << "sobelLimit = " << state.sobelLimit << endl;
                cout << "blockSize = " << state.blockSize << endl;
                cout << "sadThreshold = " << state.sadThreshold << endl;
                cout << "sobelAdd = " << state.sobelAdd << endl;
            }
        }
        
        numFrames ++;
        
        // check for new LCM messages
        NonBlockingLcm(lcm);
            
        // compute framerate
        gettimeofday( &now, NULL );
        
        elapsed = (now.tv_usec / 1000 + now.tv_sec * 1000) - 
        (start.tv_usec / 1000 + start.tv_sec * 1000);
        
        printf("\r%d frames (%lu ms) - %4.1f fps | %4.1f ms/frame", numFrames, elapsed, (float)numFrames/elapsed * 1000, elapsed/(float)numFrames);
        fflush(stdout);

        
    } // end main while loop

    printf("\n\n");
    
    destroyWindow("Input");
    destroyWindow("Input2");
    destroyWindow("Stereo");
    
    // close camera
    StopCapture(d, camera);
    StopCapture(d2, camera2);
    
    return 0;
}





void WriteVideo()
{
    printf("Writing video...\n");
    
    int endI, firstFrame = 0;
    if (recNumFrames < RINGBUFFER_SIZE)
    {
        endI = recNumFrames;
    } else {
        // our buffer is smaller than the full movie.
        // figure out where in the ringbuffer we are
        firstFrame = recNumFrames%RINGBUFFER_SIZE+1;
        if (firstFrame > RINGBUFFER_SIZE)
        {
            firstFrame = 0;
        }
        
        endI = RINGBUFFER_SIZE;
        
        printf("\nWARNING: buffer size exceeded by %d frames, which have been dropped.\n\n", recNumFrames-RINGBUFFER_SIZE);
        
    }
    
    VideoWriter recordL = SetupVideoWriter("videoL-skip-" + std::to_string(firstFrame), ringbufferL[0].size(), stereoConfig);
    
    VideoWriter recordR = SetupVideoWriter("videoR-skip-"
        + std::to_string(firstFrame), ringbufferR[0].size(),
        stereoConfig, false);
        
    // write the video
    for (int i=0; i<endI; i++)
    {
        recordL << ringbufferL[(i+firstFrame)%RINGBUFFER_SIZE];
        recordR << ringbufferR[(i+firstFrame)%RINGBUFFER_SIZE];
        
        printf("\rWriting video: (%.1f%%) -- %d/%d frames", (float)(i+1)/endI*100, i+1, endI);
        fflush(stdout);
    }
    printf("\ndone.\n");
}



/* setup a mouse callback so that the user can click on an image
 * and see where the disparity line is on the other image pair
 */
void onMouse( int event, int x, int y, int flags, void* )
{
    if( flags & CV_EVENT_FLAG_LBUTTON)
    {
        // paint a line on the image they clicked on
        lineLeftImgPosition = x;
        lineLeftImgPositionY = y;
    }
}

void DrawLines(Mat leftImg, Mat rightImg, Mat stereoImg, int lineX, int lineY, int disparity)
{
    int lineColor = 128;
    if (lineX >= 0)
    {
        // print out the values of the pixels where they clicked
        //cout << endl << endl << "Left px: " << (int)leftImg.at<uchar>(lineY, lineX)
        //    << "\tRight px: " << (int)rightImg.at<uchar>(lineY, lineX + disparity)
        //    << endl;
            
        line(leftImg, Point(lineX, 0), Point(lineX, leftImg.rows), lineColor);
        line(stereoImg, Point(lineX, 0), Point(lineX, leftImg.rows), lineColor);
        line(rightImg, Point(lineX + disparity, 0), Point(lineX + disparity, rightImg.rows), lineColor);
        
        line(leftImg, Point(0, lineY), Point(leftImg.cols, lineY), lineColor);
        line(stereoImg, Point(0, lineY), Point(leftImg.cols, lineY), lineColor);
        line(rightImg, Point(0, lineY), Point(rightImg.cols, lineY), lineColor);
    }
}

# if 0
/**
 * Function that finds and resets all connected USB Point Grey cameras.
 *
 * @retval true on successful reset, false on failure.
 */
bool ResetPointGreyCameras()
{
    libusb_context *s_libucontext;
    
    // open the libusb context
    libusb_init(&s_libucontext);
    
    libusb_device **list;

    ssize_t ret;

    ret = libusb_get_device_list (s_libucontext, &list);

    if (ret == LIBUSB_ERROR_NO_MEM || ret < 1)
    {
        printf("Failed to find device in the libusb context.\n");
        libusb_free_device_list(list, 1);
        libusb_exit(s_libucontext);
        return false;
    }
    
    int j;
    int libusb_flag = 0;
    
    for (j=0; j<ret; j++)
    {
        libusb_device *this_device = list[j];
        struct libusb_device_descriptor desc;

        if (libusb_get_device_descriptor(this_device, &desc) == 0)
        {
            // before attempting to open the device and get it's description, look at the vendor and product IDs
            // this also means that we won't attempt to open a bunch of devices we probably don't have permissions to open
            // furthermore, if the user hasn't set up permissions correctly, libusb will print out an error about the permissions
            // giving a good hint at what to do
            
            cout << desc.idVendor << endl;
            
            if (desc.idVendor == POINT_GREY_VENDOR_ID && desc.idProduct == POINT_GREY_PRODUCT_ID)
            {
                cout << "found a hit" << endl;

                libusb_device_handle *udev;
                if (libusb_open(this_device, &udev) == 0)
                {
                    cout << "resetting device." << endl;
                    // reset this device
                    cout << libusb_reset_device(udev);
                    cout << "success." << endl;
                    //libusb_close(udev);
                    libusb_flag = 1;
                } else
                {
                    cout << "failed to open device" << endl;
                }
            }
        }
    }
    
    // cleanup libusb list
    libusb_free_device_list(list, 1);
    
    libusb_exit(s_libucontext);

    if (libusb_flag == 0)
    {
        printf("Failed to reset the USB cameras.\n");
        return false;
    }
    
    return true;
    
}
#endif
