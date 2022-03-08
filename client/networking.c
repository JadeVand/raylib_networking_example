/**********************************************************************************************
*
*   raylib_networking_smaple * a sample network game using raylib and enet
*
*   LICENSE: ZLIB
*
*   Copyright (c) 2021 Jeffery Myers
*
*   Permission is hereby granted, free of charge, to any person obtaining a copy
*   of this software and associated documentation files (the "Software"), to deal
*   in the Software without restriction, including without limitation the rights
*   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
*   copies of the Software, and to permit persons to whom the Software is
*   furnished to do so, subject to the following conditions:
*
*   The above copyright notice and this permission notice shall be included in all
*   copies or substantial portions of the Software.
*
*   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
*   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
*   SOFTWARE.
*
**********************************************************************************************/

// implementation code for network game play interface

#include "networking.h"

// ensure we are using winsock2 on windows.
#ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0601
#endif

// include the network layer from enet (https://github.com/zpl-c/enet)
#define ENET_IMPLEMENTATION
#include "enet.h"

// the player id of this client
int LocalPlayerId = -1;

// the enet address we are connected to
ENetAddress address = { 0 };

// the server object we are connecting to
ENetPeer* server = { 0 };

// the client peer we are using
ENetHost* client = { 0 };

// time data for the network tick so that we don't spam the server with one update every drawing frame

// how long in seconds since the last time we sent an update
double LastInputSend = -100;

// how long to wait between updates (20 update ticks a second)
double InputUpdateInterval = 1.0f / 20.0f;

// Data about players
typedef struct
{
    // true if the player is active and valid
    bool Active;

    // the last known location of the player on the field
    Vector2 Position;
}RemotePlayer;

// The list of all possible players
// this is the local simulation that represents the current game state
// it includes the current local player and the last known data from all remote players
// the client checks this every frame to see where everyone is on the field
RemotePlayer Players[MAX_PLAYERS] = { 0 };

// All the different commands that can be sent over the network
typedef enum
{
    // Server -> Client, You have been accepted. Contains the id for the client player to use
    AcceptPlayer = 1,

    // Server -> Client, Add a new player to your simulation, contains the ID of the player and a position
    AddPlayer = 2,

    // Server -> Client, Remove a player from your simulation, contains the ID of the player to remove
    RemovePlayer = 3,

    // Server -> Client, Update a player's position in the simulation, contains the ID of the player and a position
    UpdatePlayer = 4,

    // Client -> Server, Provide an updated location for the client's player, contains the postion to update
    UpdateInput = 5,
}NetworkCommands;

// Connect to a server
void Connect()
{
    // startup the network library
    enet_initialize();

    // create a client that we will use to connect to the server
    client = enet_host_create(NULL, 1, 1, 0, 0);
    
    // set the address and port we will connect to
    enet_address_set_host(&address, "127.0.0.1");
    address.port = 4545;

    // start the connection process. Will be finished as part of our update
    server = enet_host_connect(client, &address, 1, 0);
}

// Utility functions to read data out of a packet
// Optimally this would go into a library that was shared by the client and the server

/// <summary>
/// Read one byte out of a packet, from an offset, and update that offset to the next location to read from
/// </summary>
/// <param name="packet">The packet to read from</param>
/// <param name="offset">A pointer to an offset that is updated, this should be passed to other read functions so they read from the correct place</param>
/// <returns>The byte read</returns>
uint8_t ReadByte(ENetPacket* packet, size_t* offset)
{
    // make sure we have not gone past the end of the data we were sent
    if (*offset > packet->dataLength)
        return 0;

    // cast the data to a byte so we can increment it in 1 byte chunks
    uint8_t* ptr = (uint8_t*)packet->data;

    // get the byte at the current offset
    uint8_t data = ptr[(*offset)];

    // move the offset over 1 byte for the next read
    *offset = *offset + 1;

    return data;
}

/// <summary>
/// Read a signed short from the network packet
/// Note that this assumes the packet is in the host's byte ordering
/// In reality read/write code should use ntohs and htons to convert from network byte order to host byte order, so both big endian and little endian machines can play together
/// </summary>
/// <param name="packet">The packet to read from<</param>
/// <param name="offset">A pointer to an offset that is updated, this should be passed to other read functions so they read from the correct place</param>
/// <returns>The signed short that is read</returns>
int16_t ReadShort(ENetPacket* packet, size_t* offset)
{
    // make sure we have not gone past the end of the data we were sent
    if (*offset > packet->dataLength)
        return 0;

    // cast the data to a byte at the offset
    uint8_t* data = (uint8_t*)packet->data;
    data += (*offset);

    // move the offset over 2 bytes for the next read
    *offset = (*offset) + 2;

    // cast the data pointer to a short and return a copy
    return *(int16_t*)data;
}

/// <summary>
/// Read a player position from the network packet
/// player positions are sent as two signed shorts and converted into floats for display
/// since this sample does everything in pixels, this is fine, but a more robust game would want to send floats
/// </summary>
/// <param name="packet"></param>
/// <param name="offset"></param>
/// <returns>A raylib Vector with the position in the data</returns>
Vector2 ReadPosition(ENetPacket* packet, size_t* offset)
{
    Vector2 pos = { 0 };
    pos.x = ReadShort(packet, offset);
    pos.y = ReadShort(packet, offset);

    return pos;
}

// functions to handle the commands that the server will send to the client
// these take the data from enet and read out various bits of data from it to do actions based on the command that was sent

// A new remote player was added to our local simulation
void HandleAddPlayer(ENetPacket* packet, size_t* offset)
{
    // find out who the server is talking about
    int remotePlayer = ReadByte(packet, offset);
    if (remotePlayer >= MAX_PLAYERS || remotePlayer == LocalPlayerId)
        return;

    // set them as active and update the location
    Players[remotePlayer].Active = true;
    Players[remotePlayer].Position = ReadPosition(packet, offset);

    // In a more robust game, this message would have more info about the new player, such as what sprite or model to use, player name, or other data a client would need
    // this is where static data about the player would be sent, and any inital state needed to setup the local simulation
}

// A remote player has left the game and needs to be removed from the local simulation
void HandleRemovePlayer(ENetPacket* packet, size_t* offset)
{
    // find out who the server is talking about
    int remotePlayer = ReadByte(packet, offset);
    if (remotePlayer >= MAX_PLAYERS || remotePlayer == LocalPlayerId)
        return;

    // remove the player from the simulation. No other data is needed except the player id
    Players[remotePlayer].Active = false;
}

// The server has a new position for a player in our local simulation
void HandleUpdatePlayer(ENetPacket* packet, size_t* offset)
{
    // find out who the server is talking about
    int remotePlayer = ReadByte(packet, offset);
    if (remotePlayer >= MAX_PLAYERS || remotePlayer == LocalPlayerId || !Players[remotePlayer].Active)
        return;

    // update the last known position
    Players[remotePlayer].Position = ReadPosition(packet, offset);

    // in a more robust game this message would have a tick ID for what time this information was valid, and extra info about
    // what direction the player was moving so the local simulation could do prediction and smooth out the motion
}

// process one frame of updates
void Update(double now)
{
    // if we are not connected to anything yet, we can't do anything, so bail out early
    if (server == NULL)
        return;

    // Check if we have been accepted, and if so, check the clock to see if it is time for us to send the updated position for the local player
    // we do this so that we don't spam the server with updates 60 times a second and waste bandwidth
    // in a real game we'd send our movement vector and input keys along with what the current tick index was
    // this way the server can know how long it's been since the last update and can do interpolation to know were we are between updates.
    if (LocalPlayerId >= 0 && now - LastInputSend > InputUpdateInterval)
    {
        // Pack up a buffer with the data we want to send
        uint8_t buffer[5] = { 0 }; // 5 bytes for a 1 byte command number and two bytes for each X and Y value
        buffer[0] = (uint8_t)UpdateInput;   // this tells the server what kind of data to expect in this packet
        *(int16_t*)(buffer + 1) = (int16_t)Players[LocalPlayerId].Position.x;
        *(int16_t*)(buffer + 3) = (int16_t)Players[LocalPlayerId].Position.y;

        // copy this data into a packet provided by enet (TODO : add pack functions that write directly to the packet to avoid the copy)
        ENetPacket* packet = enet_packet_create(buffer,5,ENET_PACKET_FLAG_RELIABLE);

        // send the packet to the server
        enet_peer_send(server, 0, packet);

        // NOTE enet_host_service will handle releasing send packets when the network system has finally sent them,
        // you don't have to destroy them

        // mark that now was the last time we sent an update
        LastInputSend = now;
    }

    // read one event from enet and process it
    ENetEvent Event = { 0 };

    // Check to see if we even have any events to do. Since this is a a client, we don't set a timeout so that the client can keep going if there are no events
    if (enet_host_service(client, &Event, 0) > 0)
    {
        // see what kind of event it is
        switch (Event.type)
        {
        // the server sent us some data, we should process it
        case ENET_EVENT_TYPE_RECEIVE:
        {
            // we know that all valid packets have a size >= 1, so if we get this, something is bad and we ignore it.
            if (Event.packet->dataLength < 1)
                break;

            // keep an offset of what data we have read so far
            size_t offset = 0;

            // read off the command that the server wants us to do
            NetworkCommands command = (NetworkCommands)ReadByte(Event.packet, &offset);

            // if the server has not accepted us yet, we are limited in what packets we can receive
            if (LocalPlayerId == -1)
            {
                if (command == AcceptPlayer)    // this is the only thing we can do in this state, so ignore anything else
                {
                    // See who the server says we are
                    LocalPlayerId = ReadByte(Event.packet, &offset);

                    // Make sure that it makes sense
                    if (LocalPlayerId < 0 || LocalPlayerId > MAX_PLAYERS)
                    {
                        LocalPlayerId = -1;
                        break;
                    }

                    // Force the next frame to do an update by pretending it's been a very long time since our last update
                    LastInputSend = -InputUpdateInterval;

                    // We are active
                    Players[LocalPlayerId].Active = true;

                    // Set our player at some location on the field.
                    // optimally we would do a much more robust connection negotiation where we tell the server what our name is, what we look like
                    // and then the server tells us where we are
                    // But for this simple test, everyone starts at the same place on the field
                    Players[LocalPlayerId].Position = (Vector2){ 100, 100 };
                }
            }
            else // we have been accepted, so process play messages from the server
            {
                // see what the server wants us to do
                switch (command)
                {
                case AddPlayer:
                    HandleAddPlayer(Event.packet, &offset);
                    break;

                case RemovePlayer:
                    HandleRemovePlayer(Event.packet, &offset);
                    break;

                case UpdatePlayer:
                    HandleUpdatePlayer(Event.packet, &offset);
                    break;
                }
            }
            // tell enet that it can recycle the packet data
            enet_packet_destroy(Event.packet);
            break;
        }

        // we were disconnected, we have a sad
        case ENET_EVENT_TYPE_DISCONNECT:
            server = NULL;
            LocalPlayerId = -1;
            break;
        }
    }
}

// force a disconnect by shutting down enet
void Disconnect()
{
    // close our connection to the server
    if (server != NULL)
        enet_peer_disconnect(server, 0);

    // close our client
    if (client != NULL)
        enet_host_destroy(client);

    client = NULL;
    server = NULL;

    // clean up enet
    enet_deinitialize();
}

// true if we are connected and have been accepted
bool Connected()
{
    return server != NULL && LocalPlayerId >= 0;
}

int GetLocalPlayerId()
{
    return LocalPlayerId;
}

// add the input to our local position and make sure we are still inside the field
void UpdateLocalPlayer(Vector2* movementDelta)
{
    // if we are not accepted, we can't update
    if (LocalPlayerId < 0)
        return;

    // add the movement to our location
    Players[LocalPlayerId].Position = Vector2Add(Players[LocalPlayerId].Position, *movementDelta);

    // make sure we are in bounds.
    // In a real game both the client and the server would do this to help prevent cheaters
    if (Players[LocalPlayerId].Position.x < 0)
        Players[LocalPlayerId].Position.x = 0;

    if (Players[LocalPlayerId].Position.y < 0)
        Players[LocalPlayerId].Position.y = 0;

    if (Players[LocalPlayerId].Position.x > FieldSizeWidth-PlayerSize)
        Players[LocalPlayerId].Position.x = FieldSizeWidth - PlayerSize;

    if (Players[LocalPlayerId].Position.y > FieldSizeHeight - PlayerSize)
        Players[LocalPlayerId].Position.y = FieldSizeHeight - PlayerSize;
}

// get the info for a particular player
bool GetPlayerPos(int id, Vector2* pos)
{
    // make sure the player is valid and active
    if (id < 0 || id >= MAX_PLAYERS || !Players[id].Active)
        return false;

    // copy the location
    *pos = Players[id].Position;
    return true;
}
