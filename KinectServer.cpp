/***********************************************************************
KinectServer - Server to stream 3D video data from one or more Kinect
cameras to remote clients for tele-immersion.
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

#include "KinectServer.h"

#include <iostream>
#include <Misc/FunctionCalls.h>
#include <Misc/Time.h>
#include <Misc/StandardValueCoders.h>
#include <Misc/CompoundValueCoders.h>
#include <Misc/ConfigurationFile.h>
#include <USB/Context.h>
#include <USB/DeviceList.h>
#include <IO/File.h>
#include <Comm/TCPPipe.h>
#include <Geometry/GeometryMarshallers.h>
#include <Kinect/FrameBuffer.h>

/******************************************
Methods of class KinectServer::CameraState:
******************************************/

void KinectServer::CameraState::colorStreamingCallback(const Kinect::FrameBuffer& frame)
	{
	/* Pass the frame to the color compressor: */
	colorCompressor.writeFrame(frame);
	
	/* Store the compressed frame data in the color frame triple buffer: */
	CompressedFrame& compressedFrame=colorFrames.startNewValue();
	compressedFrame.index=colorFrameIndex;
	compressedFrame.timeStamp=frame.timeStamp;
	colorFile.storeBuffers(compressedFrame.data);
	colorFrames.postNewValue();
	newColorFrameCond.signal();
	++colorFrameIndex;
	}

void KinectServer::CameraState::depthStreamingCallback(const Kinect::FrameBuffer& frame)
	{
	/* Pass the frame to the depth compressor: */
	depthCompressor.writeFrame(frame);
	
	/* Store the compressed frame data in the depth frame triple buffer: */
	CompressedFrame& compressedFrame=depthFrames.startNewValue();
	compressedFrame.index=depthFrameIndex;
	compressedFrame.timeStamp=frame.timeStamp;
	depthFile.storeBuffers(compressedFrame.data);
	depthFrames.postNewValue();
	newDepthFrameCond.signal();
	++depthFrameIndex;
	}

KinectServer::CameraState::CameraState(libusb_device* sDevice,Threads::MutexCond& sNewColorFrameCond,Threads::MutexCond& sNewDepthFrameCond)
	:camera(sDevice),
	 colorFile(16384),colorCompressor(colorFile,camera.getActualFrameSize(Kinect::FrameSource::COLOR)),
	 colorFrameIndex(0),newColorFrameCond(sNewColorFrameCond),hasSentColorFrame(false),
	 depthFile(16384),depthCompressor(depthFile,camera.getActualFrameSize(Kinect::FrameSource::DEPTH)),
	 depthFrameIndex(0),newDepthFrameCond(sNewDepthFrameCond),hasSentDepthFrame(false)
	{
	/* Extract the depth and color compressors' stream header data: */
	colorFile.storeBuffers(colorHeaders);
	depthFile.storeBuffers(depthHeaders);
	}

KinectServer::CameraState::~CameraState(void)
	{
	/* Stop streaming: */
	camera.stopStreaming();
	}

void KinectServer::CameraState::startStreaming(void)
	{
	/* Start streaming: */
	camera.startStreaming(Misc::createFunctionCall(this,&KinectServer::CameraState::colorStreamingCallback),Misc::createFunctionCall(this,&KinectServer::CameraState::depthStreamingCallback));
	}

void KinectServer::CameraState::writeHeaders(IO::File& sink) const
	{
	/* Write the color and depth compression headers: */
	colorHeaders.writeToSink(sink);
	depthHeaders.writeToSink(sink);
	
	/* Get the camera's intrinsic and extrinsic parameters: */
	Kinect::FrameSource::IntrinsicParameters ips=camera.getIntrinsicParameters();
	Kinect::FrameSource::ExtrinsicParameters eps=camera.getExtrinsicParameters();
	
	/* Write the camera parameters to the sink: */
	Misc::Marshaller<Kinect::FrameSource::IntrinsicParameters::PTransform>::write(ips.colorProjection,sink);
	Misc::Marshaller<Kinect::FrameSource::IntrinsicParameters::PTransform>::write(ips.depthProjection,sink);
	Misc::Marshaller<Kinect::FrameSource::ExtrinsicParameters>::write(eps,sink);
	}

/*****************************
Methods of class KinectServer:
*****************************/

void* KinectServer::listeningThreadMethod(void)
	{
	Threads::Thread::setCancelState(Threads::Thread::CANCEL_ENABLE);
	// Threads::Thread::setCancelType(Threads::Thread::CANCEL_ASYNCHRONOUS);
	
	while(true)
		{
		/* Wait for the next incoming connection: */
		Comm::TCPPipe* newClientSocket=0;
		try
			{
			#ifdef VERBOSE
			std::cout<<"KinectServer: Waiting for client connection"<<std::endl<<std::flush;
			#endif
			newClientSocket=new Comm::TCPPipe(listeningSocket);
			#ifdef VERBOSE
			std::cout<<"KinectServer: Connecting new client from host "<<newClientSocket->getPeerHostName()<<", port "<<newClientSocket->getPeerPortId()<<std::endl<<std::flush;
			#endif
			}
		catch(std::runtime_error err)
			{
			std::cerr<<"KinectServer: Caught exception "<<err.what()<<" while waiting for new client connection"<<std::endl;
			}
		catch(...)
			{
			std::cerr<<"KinectServer: Caught spurious exception while waiting for new client connection; terminating"<<std::endl;
			throw;
			}
		
		try
			{
			/* Send stream initialization states to the new client: */
			#ifdef VERBOSE
			std::cout<<"KinectServer: Sending stream headers to new client"<<std::endl<<std::flush;
			#endif
			newClientSocket->write<unsigned int>(0x12345678U);
			newClientSocket->write<unsigned int>(numCameras);
			for(unsigned i=0;i<numCameras;++i)
				cameraStates[i]->writeHeaders(*newClientSocket);
			newClientSocket->flush();
			
			/* Lock the client list and append the new client: */
			#ifdef VERBOSE
			std::cout<<"KinectServer: Adding new client to list of clients"<<std::endl<<std::flush;
			#endif
			{
			Threads::Mutex::Lock clientListLock(clientListMutex);
			clients.push_back(newClientSocket);
			}
			}
		catch(std::runtime_error err)
			{
			std::cerr<<"KinectServer: Disconnecting new client due to exception "<<err.what()<<std::endl<<std::flush;
			delete newClientSocket;
			}
		catch(...)
			{
			std::cerr<<"KinectServer: Disconnecting new client due to spurious exception; terminating"<<std::endl<<std::flush;
			delete newClientSocket;
			throw;
			}
		}
	
	return 0;
	}

void* KinectServer::streamingThreadMethod(void)
	{
	Threads::Thread::setCancelState(Threads::Thread::CANCEL_ENABLE);
	Threads::Thread::setCancelType(Threads::Thread::CANCEL_DEFERRED);
	
	#ifdef VERBOSE2
	std::cout<<"Meta frame "<<metaFrameIndex;
	#endif
	
	while(true)
		{
		while(numMissingDepthFrames>0||numMissingColorFrames>0)
			{
			/* Find the next missing frame that has just become available: */
			bool foundFrame=false;
			for(unsigned int i=0;!foundFrame&&i<numCameras;++i)
				{
				if(!cameraStates[i]->hasSentColorFrame&&cameraStates[i]->colorFrames.lockNewValue())
					{
					#ifdef VERBOSE2
					std::cout<<" color "<<i<<", "<<cameraStates[i]->colorFrames.getLockedValue().index<<", "<<cameraStates[i]->colorFrames.getLockedValue().timeStamp<<';';
					#endif
					
					/* Send the camera's new color frame to all connected clients: */
					{
					Threads::Mutex::Lock clientListLock(clientListMutex);
					unsigned int numClients=clients.size();
					for(unsigned int j=0;j<numClients;++j)
						{
						try
							{
							if(clients[j]->waitForData(Misc::Time(0,0)))
								{
								/* Read the disconnect request: */
								clients[j]->read<unsigned int>();
								
								/* Disconnect the client: */
								#ifdef VERBOSE
								std::cerr<<"Disconnecting client from "<<clients[j]->getPeerHostName()<<", port "<<clients[j]->getPeerPortId()<<std::endl;
								#endif
								delete clients[j];
								clients.erase(clients.begin()+j);
								--numClients;
								--j;
								}
							else
								{
								#ifdef VVERBOSE
								std::cout<<metaFrameIndex<<", "<<i*2+0<<", "<<cameraStates[i]->colorFrames.getLockedValue().timeStamp<<std::endl;
								#endif
								
								/* Write the meta frame index and frame identifier: */
								clients[j]->write<unsigned int>(metaFrameIndex);
								clients[j]->write<unsigned int>(i*2+0);
								
								/* Write the compressed color frame: */
								cameraStates[i]->colorFrames.getLockedValue().data.writeToSink(*clients[j]);
								clients[j]->flush();
								}
							}
						catch(std::runtime_error err)
							{
							std::cerr<<"Disconnecting client from "<<clients[j]->getPeerHostName()<<", port "<<clients[j]->getPeerPortId()<<" due to exception "<<err.what()<<std::endl;
							delete clients[j];
							clients.erase(clients.begin()+j);
							--numClients;
							--j;
							}
						catch(...)
							{
							std::cerr<<"Disconnecting client from "<<clients[j]->getPeerHostName()<<", port "<<clients[j]->getPeerPortId()<<" due to spurious exception; terminating"<<std::endl;
							delete clients[j];
							clients.erase(clients.begin()+j);
							--numClients;
							--j;
							throw;
							}
						}
					}
					
					cameraStates[i]->hasSentColorFrame=true;
					--numMissingColorFrames;
					foundFrame=true;
					}
				if(!cameraStates[i]->hasSentDepthFrame&&cameraStates[i]->depthFrames.lockNewValue())
					{
					#ifdef VERBOSE2
					std::cout<<" depth "<<i<<", "<<cameraStates[i]->depthFrames.getLockedValue().index<<", "<<cameraStates[i]->depthFrames.getLockedValue().timeStamp<<';';
					#endif
					
					/* Send the camera's new depth frame to all connected clients: */
					{
					Threads::Mutex::Lock clientListLock(clientListMutex);
					unsigned int numClients=clients.size();
					for(unsigned int j=0;j<numClients;++j)
						{
						try
							{
							/* Check if the client sent a disconnect request: */
							if(clients[j]->waitForData(Misc::Time(0,0)))
								{
								/* Read the disconnect request: */
								clients[j]->read<unsigned int>();
								
								/* Disconnect the client: */
								#ifdef VERBOSE
								std::cerr<<"Disconnecting client from "<<clients[j]->getPeerHostName()<<", port "<<clients[j]->getPeerPortId()<<std::endl;
								#endif
								delete clients[j];
								clients.erase(clients.begin()+j);
								--numClients;
								--j;
								}
							else
								{
								#ifdef VVERBOSE
								std::cout<<metaFrameIndex<<", "<<i*2+1<<", "<<cameraStates[i]->depthFrames.getLockedValue().timeStamp<<std::endl;
								#endif
								
								/* Write the meta frame index and frame identifier: */
								clients[j]->write<unsigned int>(metaFrameIndex);
								clients[j]->write<unsigned int>(i*2+1);
								
								/* Write the compressed depth frame: */
								cameraStates[i]->depthFrames.getLockedValue().data.writeToSink(*clients[j]);
								clients[j]->flush();
								}
							}
						catch(std::runtime_error err)
							{
							std::cerr<<"Disconnecting client from "<<clients[j]->getPeerHostName()<<", port "<<clients[j]->getPeerPortId()<<" due to exception "<<err.what()<<std::endl;
							delete clients[j];
							clients.erase(clients.begin()+j);
							--numClients;
							--j;
							}
						catch(...)
							{
							std::cerr<<"Disconnecting client from "<<clients[j]->getPeerHostName()<<", port "<<clients[j]->getPeerPortId()<<" due to spurious exception; terminating"<<std::endl;
							delete clients[j];
							clients.erase(clients.begin()+j);
							--numClients;
							--j;
							throw;
							}
						}
					}
					
					cameraStates[i]->hasSentDepthFrame=true;
					--numMissingDepthFrames;
					foundFrame=true;
					}
				}
			if(!foundFrame)
				{
				/* No frames ready; sleep until something becomes available: */
				newFrameCond.wait();
				}
			}
		
		/* Start a new meta-frame: */
		++metaFrameIndex;
		for(unsigned int i=0;i<numCameras;++i)
			{
			cameraStates[i]->hasSentColorFrame=false;
			cameraStates[i]->hasSentDepthFrame=false;
			}
		numMissingColorFrames=numCameras;
		numMissingDepthFrames=numCameras;
		
		#ifdef VERBOSE2
		std::cout<<std::endl;
		std::cout<<"Meta frame "<<metaFrameIndex;
		#endif
		}
	
	return 0;
	}

KinectServer::KinectServer(USB::Context& usbContext,Misc::ConfigurationFileSection& configFileSection)
	:numCameras(0),cameraStates(0),
	 listeningSocket(configFileSection.retrieveValue<int>("./listenPortId",26000),1)
	{
	/* Read the list of cameras: */
	std::vector<std::string> cameraNames=configFileSection.retrieveValue<std::vector<std::string> >("./cameras",std::vector<std::string>());
	numCameras=cameraNames.size();
	cameraStates=new CameraState*[numCameras];
	
	/* Enumerate all USB devices: */
	#ifdef VERBOSE
	std::cout<<"KinectServer: Enumerating Kinect camera devices on USB bus"<<std::endl;
	#endif
	USB::DeviceList usbDevices(usbContext);
	size_t numKinectCameras=usbDevices.getNumDevices(0x045eU,0x02aeU);
	unsigned int numFoundCameras=0;
	for(unsigned int i=0;i<numCameras;++i)
		{
		/* Read the camera's serial number: */
		Misc::ConfigurationFileSection cameraSection=configFileSection.getSection(cameraNames[i].c_str());
		std::string serialNumber=cameraSection.retrieveValue<std::string>("./serialNumber");
		
		/* Find a Kinect camera of the specified serial number: */
		unsigned int j;
		for(j=0;j<numKinectCameras;++j)
			{
			/* Tentatively open the Kinect camera device: */
			USB::Device cam(usbDevices.getDevice(0x045eU,0x02aeU,j));
			if(cam.getSerialNumber()==serialNumber) // Bail out if the desired device was found
				break;
			}
		if(j<numKinectCameras)
			{
			/* Create a streamer for the found camera: */
			#ifdef VERBOSE
			std::cout<<"KinectServer: Creating streamer for camera with serial number "<<serialNumber<<std::endl;
			#endif
			cameraStates[numFoundCameras]=new CameraState(usbDevices.getDevice(0x045eU,0x02aeU,j),newFrameCond,newFrameCond);
			
			/* Check if camera is to remove background: */
			if(cameraSection.retrieveValue<bool>("./removeBackground",true))
				{
				Kinect::Camera& camera=cameraStates[numFoundCameras]->camera;
				
				/* Check whether to load a previously saved background file: */
				std::string backgroundFile=cameraSection.retrieveValue<std::string>("./backgroundFile",std::string());
				if(!backgroundFile.empty())
					{
					/* Load the background file: */
					camera.loadBackground(backgroundFile.c_str());
					}
				
				/* Check whether to capture background: */
				unsigned int captureBackgroundFrames=cameraSection.retrieveValue<unsigned int>("./captureBackgroundFrames",0);
				if(captureBackgroundFrames>0)
					{
					/* Request background capture: */
					camera.captureBackground(captureBackgroundFrames,false);
					}
				
				/* Check whether to set a maximum depth value: */
				unsigned int maxDepth=cameraSection.retrieveValue<unsigned int>("./maxDepth",0);
				if(maxDepth>0)
					{
					/* Set the maximum depth: */
					camera.setMaxDepth(maxDepth,false);
					}
				
				/* Set the background removal fuzz value: */
				camera.setBackgroundRemovalFuzz(cameraSection.retrieveValue<int>("./backgroundFuzz",camera.getBackgroundRemovalFuzz()));
				
				/* Enable background removal: */
				camera.setRemoveBackground(true);
				}
			
			++numFoundCameras;
			}
		else
			std::cerr<<"Kinect camera with serial number "<<serialNumber<<" not found on USB bus"<<std::endl;
		}
	
	/* Initialize streaming state: */
	#ifdef VERBOSE
	std::cout<<"KinectServer: "<<numFoundCameras<<" Kinect cameras initialized"<<std::endl;
	#endif
	numCameras=numFoundCameras;
	metaFrameIndex=0;
	numMissingColorFrames=numCameras;
	numMissingDepthFrames=numCameras;
	
	/* Start the listening and streaming threads: */
	listeningThread.start(this,&KinectServer::listeningThreadMethod);
	if(numCameras>0)
		streamingThread.start(this,&KinectServer::streamingThreadMethod);
	
	/* Start streaming on all connected cameras: */
	for(unsigned int i=0;i<numCameras;++i)
		cameraStates[i]->startStreaming();
	}

KinectServer::~KinectServer(void)
	{
	#ifdef VERBOSE
	std::cout<<"KinectServer: Shutting down listening and streaming threads"<<std::endl;
	#endif
	
	/* Stop the listening thread: */
	try
		{
		listeningThread.cancel();
		listeningThread.join();
		}
	catch(std::runtime_error err)
		{
		std::cerr<<"Caught exception "<<err.what()<<" while shutting down listening thread"<<std::endl;
		}
	catch(...)
		{
		std::cerr<<"Caught spurious exception while shutting down listening thread"<<std::endl;
		}
	
	/* Stop the streaming thread: */
	if(numCameras>0)
		{
		try
			{
			streamingThread.cancel();
			streamingThread.join();
			}
		catch(std::runtime_error err)
			{
			std::cerr<<"Caught exception "<<err.what()<<" while shutting down streaming thread"<<std::endl;
			}
		catch(...)
			{
			std::cerr<<"Caught spurious exception while shutting down streaming thread"<<std::endl;
			}
		}
	
	/* Delete all camera states: */
	#ifdef VERBOSE
	std::cout<<"KinectServer: Disconnecting from all cameras"<<std::endl;
	#endif
	for(unsigned int i=0;i<numCameras;++i)
		delete cameraStates[i];
	delete[] cameraStates;
	
	/* Disconnect all clients: */
	#ifdef VERBOSE
	std::cout<<"KinectServer: Disconnecting all clients"<<std::endl;
	#endif
	for(std::vector<Comm::TCPPipe*>::iterator cIt=clients.begin();cIt!=clients.end();++cIt)
		{
		try
			{
			delete *cIt;
			}
		catch(std::runtime_error err)
			{
			std::cerr<<"Caught exception "<<err.what()<<" while forcefully disconnecting client from "<<(*cIt)->getPeerHostName()<<", port "<<(*cIt)->getPeerPortId()<<std::endl;
			}
		catch(...)
			{
			std::cerr<<"Caught spurious exception while forcefully disconnecting client from "<<(*cIt)->getPeerHostName()<<", port "<<(*cIt)->getPeerPortId()<<std::endl;
			}
		}
	}