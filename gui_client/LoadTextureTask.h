/*=====================================================================
LoadTextureTask.h
-----------------
Copyright Glare Technologies Limited 2019 -
=====================================================================*/
#pragma once


#include <Task.h>
#include <ThreadMessage.h>
#include <string>
class MainWindow;
class OpenGLEngine;


class TextureLoadedThreadMessage : public ThreadMessage
{
public:
	std::string tex_path;
	std::string tex_key;
};


/*=====================================================================
LoadTextureTask
---------------

=====================================================================*/
class LoadTextureTask : public Indigo::Task
{
public:
	LoadTextureTask(const Reference<OpenGLEngine>& opengl_engine_, MainWindow* main_window_, const std::string& path_);

	virtual void run(size_t thread_index);

private:
	Reference<OpenGLEngine> opengl_engine;
	MainWindow* main_window;
	std::string path;
};