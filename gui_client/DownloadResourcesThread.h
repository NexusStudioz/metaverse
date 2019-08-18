/*=====================================================================
DownloadResourcesThread.h
-------------------
Copyright Glare Technologies Limited 2016 -
Generated at 2016-01-16 22:59:23 +1300
=====================================================================*/
#pragma once


#include "../shared/ResourceManager.h"
#include "WorldState.h"
#include <MessageableThread.h>
#include <Platform.h>
#include <MyThread.h>
#include <EventFD.h>
#include <ThreadManager.h>
#include <mysocket.h>
#include <set>
#include <string>
class WorkUnit;
class PrintOutput;
class ThreadMessageSink;
class Server;



class DownloadResourceMessage : public ThreadMessage
{
public:
	DownloadResourceMessage(const std::string& URL_) : URL(URL_) {}
	std::string URL;

	IndigoAtomic processed; // zero if not processed (being downloaded) yet.
};


class ResourceDownloadedMessage : public ThreadMessage
{
public:
	ResourceDownloadedMessage(const std::string& URL_) : URL(URL_) {}
	std::string URL;
};


class ResourceDownloadingStatus : public ThreadMessage
{
public:
	ResourceDownloadingStatus(size_t num) : total_to_download(num) {}
	size_t total_to_download;
};


/*=====================================================================
DownloadResourcesThread
-------------------
Downloads any resources from the server as needed.
This thread gets sent DownloadResourceMessage from MainWindow, when a new file is needed to be downloaded.
It sends ResourceDownloadedMessage's back to MainWindow via the out_msg_queue when files are downloaded.
=====================================================================*/
class DownloadResourcesThread : public MessageableThread
{
public:
	DownloadResourcesThread(ThreadSafeQueue<Reference<ThreadMessage> >* out_msg_queue, Reference<ResourceManager> resource_manager, const std::string& hostname, int port);
	virtual ~DownloadResourcesThread();

	virtual void doRun();

private:
	ThreadSafeQueue<Reference<ThreadMessage> >* out_msg_queue;
	Reference<ResourceManager> resource_manager;
	std::string hostname;
	//std::string resources_dir;
	int port;
};
