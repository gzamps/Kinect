/***********************************************************************
KinectCamera - Wrapper class to represent the color and depth camera
interface aspects of the Kinect sensor.
Copyright (c) 2010-2011 Oliver Kreylos

This file is part of the Kinect 3D Video Capture Project (Kinect).

The Kinect 3D Video Capture Project is free software; you can
redistribute it and/or modify it under the terms of the GNU General
Public License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

The Kinect 3D Video Capture Project is distributed in the hope that it
will be useful, but WITHOUT ANY WARRANTY; without even the implied
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with the Kinect 3D Video Capture Project; if not, write to the Free
Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
02111-1307 USA
***********************************************************************/

#ifndef KINECTCAMERA_INCLUDED
#define KINECTCAMERA_INCLUDED

#include <string>
#include <Misc/Timer.h>
#include <Threads/MutexCond.h>
#include <Threads/Thread.h>
#include <Kinect/USBDevice.h>

// DEBUGGING
// #include <Misc/File.h>

/* Forward declarations: */
struct libusb_device;
struct libusb_transfer;
namespace Misc {
template <class ParameterParam>
class FunctionCall;
}
namespace IO {
class File;
}
class USBContext;
class FrameBuffer;

class KinectCamera:public USBDevice
	{
	/* Embedded classes: */
	public:
	enum Camera // Enumerated type to select one of the Kinect's built-in cameras
		{
		COLOR=0,DEPTH
		};
	enum FrameSize // Enumerated type to select color and depth frame sizes
		{
		FS_640_480=0, // 640x480 frames
		FS_1280_1024 // 1280x1024 frames
		};
	enum FrameRate // Enumerated type to select frame rates for the color and depth cameras
		{
		FR_15_HZ=0, // 15 Hz
		FR_30_HZ // 30 Hz
		};
	typedef Misc::FunctionCall<const FrameBuffer&> StreamingCallback; // Function call type for streaming color or depth image capture callback
	typedef Misc::FunctionCall<KinectCamera&> BackgroundCaptureCallback; // Function call type for completion of background capture callback
	
	private:
	struct StreamingState // Structure containing necessary state to stream color or depth frames from the respective camera
		{
		/* Elements: */
		public:
		Misc::Timer& frameTimer; // Reference to camera's frame timer
		double& frameTimerOffset; // Time offset to apply to camera's timer
		unsigned int packetFlagBase; // Base value for stream's packet header flags
		int packetSize; // Size of isochronous packets in bytes
		int numPackets; // Number of packets per transfer
		int numTransfers; // Size of transfer ring buffer to handle delays or transfer bursts
		unsigned char** transferBuffers; // Array of transfer buffers
		libusb_transfer** transfers; // Array of transfer structures
		volatile int numActiveTransfers; // Number of currently active transfers to properly handle cancellation
		
		int frameSize[2]; // Size of streamed frames in pixels
		size_t rawFrameSize; // Total size of encoded frames received from the camera
		unsigned char* rawFrameBuffer; // Double buffer to assemble an encoded frame during streaming and hold a previous frame for processing
		int activeBuffer; // Index of buffer half currently receiving frame data from the camera
		double activeFrameTimeStamp; // Time stamp for the frame currently being received
		unsigned char* writePtr; // Current write position in active buffer half
		size_t bufferSpace; // Number of bytes still to be written into active buffer half
		
		Threads::MutexCond frameReadyCond; // Condition variable to signal completion of a new frame to the decoding thread
		bool readyFrameIntact; // Flag whether the completed frame was received intact
		unsigned char* volatile readyFrame; // Pointer to buffer half containing the completed frame
		double readyFrameTimeStamp; // Time stamp of completed frame
		volatile bool cancelDecoding; // Flag to cancel the deocding thread
		Threads::Thread decodingThread; // Thread to decode raw frames into user-visible format
		
		StreamingCallback* streamingCallback; // Callback to be called when a new frame has been decoded
		
		// DEBUGGING
		//Misc::File* headerFile;
		
		/* Constructors and destructors: */
		public:
		StreamingState(libusb_device_handle* handle,unsigned int endpoint,Misc::Timer& sFrameTimer,double& sFrameTimerOffset,int sPacketFlagBase,int sPacketSize,const unsigned int sFrameSize[2],size_t sRawFrameSize,StreamingCallback* sStreamingCallback); // Prepares a streaming state for streaming
		~StreamingState(void); // Cleanly stops streaming and destroys the streaming state
		
		/* Methods: */
		static void transferCallback(libusb_transfer* transfer); // Callback called when a USB transfer completes or is cancelled
		};
	
	/* Elements: */
	public:
	static const unsigned short invalidDepth=0x07ffU; // The depth value indicating an invalid (or removed) pixel
	private:
	FrameSize frameSizes[2]; // Selected frame sizes for the color and depth cameras
	FrameRate frameRates[2]; // Selected frame rates for the color and depth cameras
	unsigned short messageSequenceNumber; // Incrementing sequence number for command messages to the camera
	Misc::Timer frameTimer; // Free-running timer to time-stamp depth and color frames
	double frameTimerOffset; // Time offset to apply to cameras' timers
	bool compressDepthFrames; // Flag whether to request RLE/differential compressed depth frames from the depth camera
	StreamingState* streamers[2]; // Streaming states for color and depth frames
	unsigned int numBackgroundFrames; // Number of background frames left to capture
	unsigned short* backgroundFrame; // Frame containing minimal depth values for a captured background
	BackgroundCaptureCallback* backgroundCaptureCallback; // Function to call upon completion of background capture
	bool removeBackground; // Flag whether to remove background information during frame processing
	short int backgroundRemovalFuzz; // Fuzz value for background removal (positive values: more aggressive removal)
	
	// DEBUGGING
	//Misc::File* headerFile;
	
	/* Private methods: */
	size_t sendMessage(unsigned short messageType,const unsigned short* messageData,size_t messageSize,void* replyBuffer,size_t replyBufferSize); // Sends a general message to the camera device; returns reply size in bytes
	bool sendCommand(unsigned short command,unsigned short value); // Sends a command message to the camera device; returns true if command was processed properly
	void* colorDecodingThreadMethod(void); // The color decoding thread method
	void* depthDecodingThreadMethod(void); // The depth decoding thread method
	void* compressedDepthDecodingThreadMethod(void); // The depth decoding thread method for RLE/differential-compressed frames
	
	/* Constructors and destructors: */
	public:
	KinectCamera(libusb_device* sDevice); // Creates a Kinect camera wrapper around the given USB device, which is assumed to be a Kinect camera
	KinectCamera(USBContext& usbContext,size_t index =0); // Opens the index-th Kinect camera device on the given USB context
	~KinectCamera(void); // Destroys the camera
	
	/* Methods: */
	void setFrameSize(int camera,FrameSize newFrameSize); // Sets the frame size of the color or depth camera for the next streaming operation
	FrameSize getFrameSize(int camera) // Returns the selected frame size of the color or depth camera
		{
		return frameSizes[camera];
		}
	const unsigned int* getActualFrameSize(int camera) const; // Returns the selected frame size of the color or depth camera in pixels
	void setFrameRate(int camera,FrameRate newFrameRate); // Sets the frame rate of the color or depth camera for the next streaming operation
	FrameRate getFrameRate(int camera) const // Returns the selected frame rate of the color or depth camera
		{
		return frameRates[camera];
		}
	unsigned int getActualFrameRate(int camera) const; // Returns the selected frame rate of the color or depth camera in Hz
	void resetFrameTimer(double newFrameTimerOffset =0.0); // Resets the frame timer to zero
	void setCompressDepthFrames(bool newCompressDepthFrames); // Enables or disables depth frame compression for the next streaming operation
	void startStreaming(StreamingCallback* newColorStreamingCallback,StreamingCallback* newDepthStreamingCallback); // Installs the given streaming callback and starts receiving color and depth data from the camera
	void captureBackground(unsigned int newNumBackgroundFrames,bool replace,BackgroundCaptureCallback* newBackgroundCaptureCallback =0); // Captures the given number of frames to create a background removal buffer and calls optional callback upon completion
	void loadBackground(const char* fileNamePrefix); // Loads a background removal buffer from a file with the given prefix
	void loadBackground(IO::File& file); // Ditto, from already opened file
	void setMaxDepth(unsigned int newMaxDepth,bool replace =false); // Sets a depth value beyond which all pixels are considered background
	void saveBackground(const char* fileNamePrefix); // Saves the current background frame to a file with the given prefix
	void setRemoveBackground(bool newRemoveBackground); // Enables or disables background removal
	bool getRemoveBackground(void) const // Returns the current background removal flag
		{
		return removeBackground;
		}
	void setBackgroundRemovalFuzz(int newBackgroundRemovalFuzz); // Sets the fuzz value for background removal
	int getBackgroundRemovalFuzz(void) const // Returns the current background removal fuzz value
		{
		return backgroundRemovalFuzz;
		}
	void stopStreaming(void); // Stops streaming; blocks until all pending transfers have either completed or been cancelled
	};

#endif