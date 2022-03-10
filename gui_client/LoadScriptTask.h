/*=====================================================================
LoadScriptTask.h
----------------
Copyright Glare Technologies Limited 2022 -
=====================================================================*/
#pragma once


#include <Task.h>
#include <ThreadMessage.h>
#include <string>
#include <vector>
class MainWindow;
class WinterShaderEvaluator;


class ScriptLoadedThreadMessage : public ThreadMessage
{
public:
	std::string script;
	Reference<WinterShaderEvaluator> script_evaluator;
};


/*=====================================================================
LoadScriptTask
--------------

=====================================================================*/
class LoadScriptTask : public glare::Task
{
public:
	LoadScriptTask();
	virtual ~LoadScriptTask();

	virtual void run(size_t thread_index);

	std::string script_content;
	MainWindow* main_window;
};
