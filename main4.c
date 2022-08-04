#include <stdio.h>
#include <stdlib.h>		//malloc()
#include <unistd.h>		//sleep()
#include <stdbool.h>	//bool
#include <math.h>		//floor()
#include <time.h>		//time()
#include <stdatomic.h>	//atomic read/write
#include <sndfile.h>	//sndfile
#include <portaudio.h>	//portaudio
#include "paUtils.h"	//portaudio utility functions
#include "autorhym.h"	//declares struct Flanger and function

#define BLK_LEN	1024	//block length for block processing

//PortAudio callback structure
struct PABuf {
	float *ifbuf;
	float *ofbuf;
	int num_chan;
	int next_frame;
	int total_frame;
	atomic_bool done;
	struct Automus *pam;
};

//PortAudio callback function protoype
static int paCallback( const void *inputBuffer, void *outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void *userData );

int main(int argc, char *argv[])
{
	char *ofile;
	int N, C, ocount;
	float *ifbuf, *ofbuf; //frame buffers
    SNDFILE *osndfile;
    SF_INFO osfinfo;
	struct PABuf paBuf; //PortAudio data struct
	struct Automus am;
    PaStream *stream;

	//usage and parse command line
	if (argc != 2){
		printf("Usage: %s output.wav\n",argv[0]);
		return -1;
	}

	//initialize pointers to files
	ofile = argv[1];

	//make sure output file has same stats as input file
	osfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
	osfinfo.channels = 1;
	osfinfo.samplerate = 48000;

	//open output file
	if ((osndfile = sf_open(ofile,SFM_WRITE,&osfinfo)) == NULL){
		printf("ERROR: could not open file %s\n",argv[1]);
		return -1;
	}

	//user sets interval 1
	am.beatsperbar = user_beats();
	if ((am.beatsperbar > 32) || (am.beatsperbar < 1)){
		printf("Please input a whole number between 1 and 32\n");
		return -1;
	}

	//user sets interval 2
	am.subdivisions = user_subdivisions();
	if ((am.subdivisions > 4) || (am.subdivisions < 1)){
		printf("Please input a whole number between 1 and 4\n");
		return -1;
	}

	//user sets bpm
	am.bpm = user_bpm();
	if ((am.bpm > 480) || (am.bpm < 30)){
		printf("Please input a tempo between 30 and 480 beats per minute\n");
		return -1;
	}

	srand(time(0));
	am.bog = floor(osfinfo.samplerate/(am.bpm/60.0)); //quarter note beat duration in samples
	am.measuredur = am.beatsperbar/(am.bpm/60.0); //measure duration in seconds
	osfinfo.frames = am.bog * am.beatsperbar;

	//set N to number of frames and C to number of channels
	N = osfinfo.frames;
	C = osfinfo.channels;
	if (C > 1){ //only mono files allowed
		printf("ERROR: Please use a mono audio file\n");
		return -1;
	}

	//mallocate buffers
	ifbuf = (float *)calloc(N * C, sizeof(float));
	if (ifbuf == NULL){
		printf("ERROR: Returned pointer to ifbuf was null\n");
		return -1;
	}

	ofbuf = (float *)malloc(N * C * sizeof(float));
	if (ofbuf == NULL){
		printf("ERROR: Returned pointer to ofbuf was null\n");
		return -1;
	}

	//initialize Port Audio data struct
	paBuf.ifbuf = ifbuf;
	paBuf.ofbuf = ofbuf;
	paBuf.num_chan = osfinfo.channels;
	paBuf.next_frame = 0;
	paBuf.total_frame = osfinfo.frames;
	paBuf.done = false;

	am.fs = osfinfo.samplerate;
	am.notedur = am.bog;

	am.sine_phase = 0;
	am.sine_f0 = 440;
	am.counter = 0;

	printf("Generating one measure of %d/4\nSmallest rhythm is 1/%d note\nTempo is %dbpm\nAudio Duration: %f seconds\n",am.beatsperbar,(am.subdivisions*4),am.bpm,am.measuredur);

	//pass our auto music structure to the Port Audio structure
	paBuf.pam = &am;

    //start up Port Audio
    printf("Starting PortAudio\n");
    stream = startupPa(1, osfinfo.channels, 
      osfinfo.samplerate, BLK_LEN, paCallback, &paBuf);

	//sleep and let callback process audio until done 
    while (!paBuf.done) {
    	printf("%d\n", paBuf.next_frame);
    	sleep(1);
    }

    //shut down Port Audio
    shutdownPa(stream);

    //write output buffer to WAV file
	ocount = sf_writef_float(osndfile, ofbuf, N);
	if (ocount != osfinfo.frames){
		printf("ERROR: output count does not equal number of frames\n");
		return -1;
	}

	//close WAV files 
	//free allocated storage
	sf_close(osndfile);

	// free(ifbuf);
	free(ofbuf);

	//permission to feel good
	printf("Successfully closed files and freed memory!\n");

	return 0;
}

static int paCallback(
	const void *inputBuffer, 
	void *outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void *userData)
{
    //cast data passed via paCallback to our struct
    struct PABuf *p = (struct PABuf *)userData; 
    //cast input and output buffers */
    float *output = (float *)outputBuffer;
	//since blocks are short, just declare single-channel arrays
	double idbuf[BLK_LEN], odbuf[BLK_LEN];
	int N = framesPerBuffer;
	int C = p->num_chan;
	//local pointers to ifbuf[] and ofbuf[]
    float *ifbuf = p->ifbuf + p->next_frame*C;
    float *ofbuf = p->ofbuf + p->next_frame*C;

	//zero PortAudio output buffer for:
	//partial output buffer
	//or call to PortAudio after done == true (after all input data has been processed)
	for (int i=0; i<N; i++) {
		output[i] = 0;
	}

	//return if done
	if (p->done == true) {
		 return 0;
	}

	//adjust N if last frame is partial frame
	if (p->next_frame + N > p->total_frame) {
		N = p->total_frame - p->next_frame;
	}

	//pass input buffer into the double version
	for (int i = 0; i < N; i++){
		idbuf[i] = ifbuf[i];
	}

	sine(&odbuf[0], N, p->pam);

	//pass the output into the float version
	for (int i = 0; i < N; i++){
		ofbuf[i] = odbuf[i];
	}

	//copy ofbuf[] to portaudio output buffer 
	for (int i = 0; i < N; i++){
		output[i] = ofbuf[i];
	}

	//increment next_frame counter and check is done
	p->next_frame += N;
	if (p->next_frame >= p->total_frame) {
		p->done = true;
	}

	return 0;
}