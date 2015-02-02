﻿#include "RaceController.h"
#include "Adafruit_TCS34725.h"
#include <cpprest/basic_types.h>
#include "CommUDP.h"
#include "CommTCP.h"
#include "NoIndicator.h"
#include "ColorTrafficLight.h"

char* toHex(rgbc values);
const char *st2s(std::stringstream* stream, char *result);

const int ticksPerPreRaceStatus = 2000;
const int ticksToAllowRaceStartFromGo = 5000;

const char* raceStatusNames[] = { "Prep Ready to Start", "Ready to Start",
	"Ready", "Set", "Go", "Racing", "Off-Track", "Final Lap", "Winner",
	"Show Winner", "Waiting" };

RaceController::RaceController()
{
}

void RaceController::Initialize()
{
	raceStatus = WAITING;
	
	//indicator = new NoIndicator(); 
	//indicator = new ColorRGB(D9, D10, D11); // RGB LED (if digital pins were available)
	indicator = new ColorTrafficLight(A0, A1, A2); 

	indicator->SetColor(RED);

	tracks = vector<Track>(trackCount);
	for (int trackIndex = 0; trackIndex < trackCount; trackIndex++)
	{
		int trackId = trackIndex + trackStart;
		tracks[trackIndex] = Track(this, trackId, useColorSensors, 0, 4);
		tracks[trackIndex].Initialize();
	}

	reporter = new CommUDP(multicastAddress, multicastMask);
	reporter->Initialize();

	controller = new CommTCP("127.0.0.1");
	controller->Initialize();

	indicator->SetColor(GREEN);
}

void RaceController::Tick()
{
	//check on sensors
	for (int trackIndex = 0; trackIndex < trackCount; trackIndex++)
	{
		tracks[trackIndex].Tick();
	}

	//affect status
	StatusCheck();
	
	//report status changes
	reporter->Tick();

	if (reporter->lastBytesReceived > 0)
	{
		Log(reporter->recentInput);
	}

	//show status/indicators
	indicator->Tick();
}

// return if the status is any racing mode
bool RaceController::IsRacing()
{
	return raceStatus >= RACING && raceStatus <= WINNER;
}

bool RaceController::IsRacingOrPostRace(int ticks)
{
	return raceStatus >= RACING;
}

bool RaceController::IsInCountdown(bool includeGo)
{
	return raceStatus >= READY && includeGo ? (raceStatus <= GO) : (raceStatus < GO);
}

// blink the indicator
void RaceController::Blip(Color color)
{
	indicator->Blip(25, color);
}

// a car has passed the 1st positional sensor on a track
bool RaceController::TrackLapChanged(Track* track)
{
	if (raceStatus == RACING && track->lap == raceLaps - 1)
	{
		this->raceStatus = FINAL_LAP;
		SendRace(track->trackId, "finallap", track->trackId);
	}

	if (raceStatus == FINAL_LAP && track->lap == raceLaps)
	{
		this->raceStatus = WINNER;
		carsFinished = carsFinished + 1;

		if (carsFinished == 1)
		{
			trackStatusId = track->trackId;
			SendRace(track->trackId, "winner", track->trackId);
		}

		if (carsFinished > 0) 
		{
			raceStatus = SHOW_WINNER;
		}
	}

	return track->lap == raceLaps;
}

int RaceController::GetRaceTime(int finishLine)
{
	return finishLine - ticksRaceStarted;
}

void RaceController::TrackReady(Track* track)
{
	if (tracksReady < trackCount)
	{
		tracksReady = tracksReady + 1;
		indicator->Blip(125, GREEN);
	}
}

void RaceController::Disqualify(Track* track)
{
	trackStatusId = track->trackId;
	raceStatus = DISQUALIFY;
	Log("Disqualified");
}

void RaceController::StartRace(int ticks)
{
	ticksRaceStarted = ticks;

	for (int trackIndex = 0; trackIndex < trackCount; trackIndex++)
	{
		tracks[trackIndex].StartRace(ticks);
	}

	carsFinished = 0;
}

// check and/or modify the status
void RaceController::StatusCheck()
{
	int ticks = GetTickCount();
	
	//if we are not racing yet and either at 'GO!' status or waited too long
	if (raceStatus < RACING && (raceStatus == GO || ticks >= lastRaceStatusTicks + ticksPerPreRaceStatus))
	{
		//progress the race status
		raceStatus = static_cast<RaceStatus> (raceStatus + 1);

		//if at 'racing'... restart the race
		if (raceStatus == RACING)
		{
			StartRace(ticks);
		}
	}

	RaceStatus reportableRaceStatus = raceStatus;

	bool anyCarsOffTrack = false;
	for (int i = 0; i < trackCount; i++)
	{
		if (tracks[i].isOfftrack)
		{
			anyCarsOffTrack = true;
			break;
		}
	}

	if (reportableRaceStatus == RACING && anyCarsOffTrack)
	{
        if (lastRaceStatus != OFF_TRACK)
        {
            lastStatusChangeDateTime = GetTickCount();
        }
		reportableRaceStatus = OFF_TRACK;
	}

	bool statusChanged = reportableRaceStatus != lastRaceStatus;

    if (!statusChanged)
	{
        DWORD diff = (GetTickCount() - lastStatusChangeDateTime);
        if (reportableRaceStatus == OFF_TRACK &&  diff > 10000) {
            raceStatus = WAITING;
            reportableRaceStatus = WAITING;
        }
        else {
            return;
        }

//		return;
	}

    lastStatusChangeDateTime = GetTickCount();

	lastRaceStatusTicks = ticks;
	lastRaceStatus = reportableRaceStatus;

	Log("Status=%s\n", const_cast<char*>(raceStatusNames[reportableRaceStatus]));
	SendRace(0, "status", const_cast<char*>(raceStatusNames[reportableRaceStatus]));

	// Set Indicator for racing status -----------------------------------------------------

	switch (reportableRaceStatus)
	{
	case PREP_READY_TO_START: 
		indicator->Flash(vector<Color>{ WHITE, BLACK }, 250);
		break;
	case READY_TO_START:
		indicator->Flash(vector<Color>{ WHITE, BLACK }, 250);
		break;
	case READY:
		indicator->SetColor(RED);
		break;
	case SET: 
		indicator->SetColor(YELLOW);
		break;
	case GO: 
		indicator->SetColor(GREEN);
		break;
	case RACING:
		indicator->SetColor(GREEN);
		break;
	case OFF_TRACK:
		indicator->Flash(vector<Color>{ YELLOW, BLACK }, 250);
		break;
	case FINAL_LAP:
		indicator->Flash(vector<Color>{ YELLOW, RED }, 250);
		break;
	case WINNER:
		indicator->Flash(vector<Color>{ WHITE, BLACK }, 50);
		break;
	case SHOW_WINNER:
		indicator->Flash(vector<Color>{ GREEN, BLACK, (trackStatusId == 2 ? GREEN : BLACK), BLACK, BLACK, BLACK}, 125);
		break;
	case WAITING:
		indicator->SetColor(BLACK);
		break;
	case DISQUALIFY:
		indicator->Flash(vector<Color>{ RED, BLACK, (trackStatusId == 2 ? RED : BLACK), BLACK, BLACK, BLACK}, 125);
		break;
	default: 
		break;
	}
}

// color sensor has a significant change for a persistent time
void RaceController::ColorChanged(Track* track)
{
	char* hex = toHex(track->GetAdjustedRGBValue());
	Log("Color sensor %d:significant:%s\n", track->trackId, hex);

	char message[100];
	sprintf(message, "{ \"track\": %d, \"color\": \"%s\" }", track->trackId, hex);

	SendRaw(message);
	SendRace(track->trackId, "color", hex);

	Log(message);
	Log("\n");
}

//Communication / Logging section

int RaceController::SendRaw(char *message)
{
	return SendDirect(message, 12345);
}

int RaceController::SendRace(int track, char *key, char *value)
{
	char trackMessage[14];
	if (track > 0)
	{
		sprintf(trackMessage, "\"track\": %d, ", track);
	}
	else
	{
		trackMessage[0] = 0;
	}

	char message[100];
	sprintf(message, "{ %s\"%s\": \"%s\" }", trackMessage, key, value);

	return SendDirect(message, 12346);
}

int RaceController::SendRace(int track, char* key, int value)
{
	char trackMessage[14];
	if (track > 0)
	{
		sprintf(trackMessage, "\"track\": %d, ", track);
	}
	else
	{
		trackMessage[0] = 0;
	}

	char message[100];
	sprintf(message, "{ %s\"%s\": %d }", trackMessage, key, value);

	return SendDirect(message, 12346);
}

int RaceController::SendDirect(char *message, unsigned short port)
{
	return reporter->Send(message, port);
}

const char *st2s(std::stringstream* stream, char *result) {
	std::string msg1 = stream->str();
	const char * result1 = msg1.c_str();
	for (int i = 0; i < sizeof(msg1); i++)
	{
		result[i] = result1[i];
	}

	return result;
}

char* toHex(rgbc values)
{
	std::stringstream stream;
	stream << "#";

	stream << (values.r<16 ? "0" : "") << std::hex << values.r;
	stream << (values.g<16 ? "0" : "") << std::hex << values.g;
	stream << (values.b<16 ? "0" : "") << std::hex << values.b;

	char *result = new char[10];
	st2s(&stream, result);

	return result;
}