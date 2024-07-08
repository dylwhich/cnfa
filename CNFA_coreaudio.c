//Copyright 2015-2020 <>< Charles Lohr under the MIT/x11, NewBSD or ColorChord License.  You choose.


#include "CNFA.h"
#include "os_generic.h"
#include <stdlib.h>

#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioQueue.h>
#include <stdio.h>
#include <string.h>

#define BUFFERSETS 3


// https://developer.apple.com/library/archive/documentation/MusicAudio/Conceptual/CoreAudioOverview/CoreAudioEssentials/CoreAudioEssentials.html


struct CNFADriverCoreAudio
{
	void (*CloseFn)( void * object );
	int (*StateFn)( void * object );
	CNFACBType callback;
	short channelsPlay;
	short channelsRec;
	int spsPlay;
	int spsRec;
	void * opaque;

	char * sourceNamePlay;
	char * sourceNameRec;

	og_thread_t thread;
	AudioQueueRef   play;
	AudioQueueRef   rec;

	int buffer;
	//More fields may exist on a per-sound-driver basis
};


int CNFAStateCoreAudio( void * v )
{
	struct CNFADriverCoreAudio * soundobject = (struct CNFADriverCoreAudio *)v;
	return ((soundobject->play)?2:0) | ((soundobject->rec)?1:0);
}

void CloseCNFACoreAudio( void * v )
{
	struct CNFADriverCoreAudio * r = (struct CNFADriverCoreAudio *)v;
	if( r )
	{
		OGCancelThread( r->thread );

		OGUSleep(2000);

		if( r->play )
		{
			AudioQueueDispose(r->play);
			r->play = 0;
		}

		if( r->rec )
		{
			AudioQueueDispose(r->rec);
			r->rec = 0;
		}

		OGUSleep(2000);

		if( r->sourceNamePlay ) free( r->sourceNamePlay );
		if( r->sourceNameRec ) free( r->sourceNameRec );
		free( r );
	}
}

static void CoreAudioOutputCallback(void *userdata, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer)
{
	// Write our data to the input buffer
	struct CNFADriverCoreAudio * r = (struct CNFADriverCoreAudio*)userdata;
	if (!r->play)
	{
		return;
	}

	//short buf[inBuffer->mAudioDataBytesCapacity / sizeof(short)];

	r->callback((struct CNFADriver*)r, (short*)inBuffer->mAudioData, NULL, inBuffer->mAudioDataBytesCapacity / sizeof(short), 0);
	// Assume the buffer was completely filled?
	inBuffer->mAudioDataByteSize = inBuffer->mAudioDataBytesCapacity;

	if (inBuffer->mPacketDescriptionCapacity > 0)
	{
		inBuffer->mPacketDescriptionCount = 1;
		inBuffer->mPacketDescriptions[0].mDataByteSize = inBuffer->mAudioDataBytesCapacity;
		inBuffer->mPacketDescriptions[0].mStartOffset = 0;
		inBuffer->mPacketDescriptions[0].mVariableFramesInPacket = 0;
	}
}

static void CoreAudioInputCallback(void *userdata, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer, const AudioTimeStamp *inStartTime, UInt32 inNumberPacketDescriptions, const AudioStreamPacketDescription *inPacketDescs)
{
	// Read our data from the output buffer
	struct CNFADriverCoreAudio * r = (struct CNFADriverCoreAudio*)userdata;
	r->callback((struct CNFADriver*)r, NULL, (const short*)inBuffer->mAudioData, 0, inBuffer->mAudioDataBytes / sizeof(short));
}

void * InitCNFACoreAudio( CNFACBType cb, const char * your_name, int reqSPSPlay, int reqSPSRec, int reqChannelsPlay, int reqChannelsRec, int sugBufferSize, const char * outputSelect, const char * inputSelect, void * opaque )
{
	const char * title = your_name;

	struct CNFADriverCoreAudio * r = (struct CNFADriverCoreAudio *)malloc( sizeof( struct CNFADriverCoreAudio ) );

	r->CloseFn = CloseCNFACoreAudio;
	r->StateFn = CNFAStateCoreAudio;
	r->callback = cb;
	r->opaque = opaque;
	r->spsPlay = reqSPSPlay;
	r->spsRec = reqSPSRec;
	r->channelsPlay = reqChannelsPlay;
	r->channelsRec = reqChannelsRec;
	r->sourceNamePlay = outputSelect?strdup(outputSelect):0;
	r->sourceNameRec = inputSelect?strdup(inputSelect):0;

	r->play = 0;
	r->rec = 0;
	r->buffer = sugBufferSize;

	printf ("CoreAudio: from: [O/I] %s/%s (%s) / (%d,%d)x(%d,%d) (%d)\n", r->sourceNamePlay, r->sourceNameRec, title, r->spsPlay, r->spsRec, r->channelsPlay, r->channelsRec, r->buffer );

	int bufBytesPlay = r->buffer * sizeof(short) * r->channelsPlay;
	int bufBytesRec = r->buffer * sizeof(short) * r->channelsRec;

	if( r->channelsPlay )
	{
		AudioStreamBasicDescription playDesc = {0};
		playDesc.mAudioFormat = kAudioFormatLinearPCM;
		playDesc.mSampleRate = r->spsPlay;
		playDesc.mBitsPerChannel = 16;
		// Bytes per channel, multiplied by number of channels
		playDesc.mBytesPerFrame = r->channelsPlay * (playDesc.mBitsPerChannel / 8);
		// Variable packet size
		playDesc.mBytesPerPacket = 0;
		// Always 1 for uncompressed audio
		playDesc.mFramesPerPacket = 1;

		OSStatus result = AudioQueueNewOutput(&playDesc, CoreAudioInputCallback, (void*)r, NULL /* Default run loop*/, NULL /* Equivalent to kCFRunLoopCommonModes */, 0 /* flags, reserved*/, &r->play);

		if( 0 != result )
		{
			fprintf(stderr, __FILE__": (PLAY) AudioQueueNewOutput() failed: %s\n", strerror(result));
			goto fail;
		}

	}

	if( r->channelsRec )
	{
		AudioStreamBasicDescription recDesc = {0};
		recDesc.mAudioFormat = kAudioFormatLinearPCM;
		recDesc.mSampleRate = r->spsRec;
		recDesc.mBitsPerChannel = 16;
		// Bytes per channel, multiplied by number of channels
		recDesc.mBytesPerFrame = r->channelsRec * (recDesc.mBitsPerChannel / 8);
		// Variable packet size
		recDesc.mBytesPerPacket = 0;
		// Always 1 for uncompressed audio
		recDesc.mFramesPerPacket = 1;

		OSStatus result = AudioQueueNewInput(&recDesc, CoreAudioInputCallback, (void*)r, NULL, NULL, 0, &r->rec);

		if (0 != result)
		{
			fprintf(stderr, __FILE__": (RECORD) AudioQueueNewInput() failed: %s\n", strerror(result));
			goto fail;
		}
	}

	printf( "CoreAudio initialized.\n" );

	return r;

fail:
	if( r )
	{
		if( r->play ) AudioQueueDispose (r->play);
		if( r->rec ) AudioQueueDispose (r->rec);
		free( r );
	}
	return 0;
}



REGISTER_CNFA( CoreAudioCNFA, 10, "COREAUDIO", InitCNFACoreAudio );
