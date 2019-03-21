#include <iostream>
#include <string.h>
#include "MemStream.h"
#include "DebugMeowMeow.hpp"
#include "RayMessenger.hpp"

using namespace std;
namespace vtkh
{

RayMessenger::RayMessenger(MPI_Comm comm)
  : Messenger(comm)
{
}

void
RayMessenger::RegisterMessages(int mSz,
                               int nMsgRecvs,
                               int nICRecvs,
                               int nDSRecvs)
{
    numMsgRecvs = nMsgRecvs;
    numSLRecvs = nICRecvs;
    numDSRecvs = nDSRecvs;

    // Msgs are handled as vector<int>.
    // Serialization of msg consists: size_t (num elements) +
    // sender rank + message size.
    int msgSize = sizeof(size_t);
    msgSize += sizeof(int); // sender rank.
    msgSize += (mSz * sizeof(int));

    //During particle advection, the IC state is only serialized.
    slSize = 256; // avoids splitting send buffer into multiple chunks
    //slSize = sizeof(Ray);
    //slsPerRecv = 64;
    //slsPerRecv = 640; //works
    slsPerRecv = 639;

    int dsSize = 2 * sizeof(int);

    this->RegisterTag(RayMessenger::MESSAGE_TAG, numMsgRecvs, msgSize);
    this->RegisterTag(RayMessenger::RAY_TAG, numSLRecvs, slSize * slsPerRecv);

    this->InitializeBuffers();
}

void
RayMessenger::SendMsg(int dst, vector<int> &msg)
{
    MemStream *buff = new MemStream;

    //Write data.
    vtkh::write(*buff, rank);
    vtkh::write(*buff, msg);

    SendData(dst, RayMessenger::MESSAGE_TAG, buff);
//    MsgCnt.value++;
//    CommTime.value += visitTimer->StopTimer(timerHandle, "SendMsg");
}

void
RayMessenger::SendAllMsg(vector<int> &msg)
{
    for (int i = 0; i < nProcs; i++)
        if (i != rank)
        {
            DBG("          ***************** SendMsg to "<<i<<" "<<msg<<endl);
            SendMsg(i, msg);
        }
}

bool
RayMessenger::RecvAny(vector<MsgCommData> *msgs,
                      std::vector<Ray> *rays,
                      bool blockAndWait)
{
    set<int> tags;
    if(msgs)
    {
      tags.insert(RayMessenger::MESSAGE_TAG);
      msgs->resize(0);
    }
    if(rays)
    {
      tags.insert(RayMessenger::RAY_TAG);
      rays->resize(0);
    }

    if (tags.empty())
        return false;

    vector<pair<int, MemStream *> > buffers;
    if (! RecvData(tags, buffers, blockAndWait))
        return false;

//    int timerHandle = visitTimer->StartTimer();

    for (size_t i = 0; i < buffers.size(); i++)
    {
        if (buffers[i].first == RayMessenger::MESSAGE_TAG)
        {
          int sendRank;
          vector<int> m;
          vtkh::read(*buffers[i].second, sendRank);
          vtkh::read(*buffers[i].second, m);

          MsgCommData msg(sendRank, m);

          msgs->push_back(msg);
        }
        else if (buffers[i].first == RayMessenger::RAY_TAG)
        {
          int num, sendRank;
          vtkh::read(*buffers[i].second, sendRank);
          vtkh::read(*buffers[i].second, num);
          rays->resize(num);
          for (int j = 0; j < num; j++)
          {
            vtkh::read(*(buffers[i].second), (*rays)[j]);
          }
          std::cout<<"["<<rank<<"] <-- ["<<sendRank<<"] "<<rays->size()<<"\n";
        }

        delete buffers[i].second;
    }

//    CommTime.value += visitTimer->StopTimer(timerHandle, "RecvAny");
    return true;
}

bool
RayMessenger::RecvMsg(vector<MsgCommData> &msgs)
{
    return RecvAny(&msgs, NULL, false);
}

void RayMessenger::SendRays(int dst, std::vector<Ray> &rays)
{
    if (dst == rank)
    {
        cerr<<"Error. Sending IC to yourself"<<endl;
        return;
    }
    if (rays.empty())
        return;

    MemStream *buff = new MemStream;
    vtkh::write(*buff, rank);
    const int num = rays.size();
    vtkh::write(*buff, num);
    for (auto &ray : rays)
    {
        //std::cout<<"writing "<<ray<<"\n";
        vtkh::write(*buff, ray);
    }
    SendData(dst, RayMessenger::RAY_TAG, buff);
    rays.clear();
}

void RayMessenger::SendRays(std::map<int, std::vector<Ray>> &ray_map)
{
    for (auto mit = ray_map.begin(); mit != ray_map.end(); mit++)
        if (! mit->second.empty())
            SendRays(mit->first, mit->second);
}

bool RayMessenger::RecvRays(std::vector<Ray> &rays)
{
    return RecvAny(NULL, &rays, false);
}

} // namespace vtkh
