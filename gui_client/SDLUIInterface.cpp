/*=====================================================================
SDLUIInterface.cpp
------------------
Copyright Glare Technologies Limited 2022 -
=====================================================================*/
#include "SDLUIInterface.h"


#include "GUIClient.h"
#include <utils/ConPrint.h>
#include <graphics/ImageMap.h>
#include <graphics/TextRenderer.h>
#include <SDL.h>


void SDLUIInterface::appendChatMessage(const std::string& msg)
{
}

void SDLUIInterface::clearChatMessages()
{
}

bool SDLUIInterface::isShowParcelsEnabled() const
{
	return false;
}

void SDLUIInterface::updateOnlineUsersList()
{
}

void SDLUIInterface::showHTMLMessageBox(const std::string& title, const std::string& msg)
{
	SDL_MessageBoxData data;
	data.title = title.c_str();
	data.message = msg.c_str();
	int buttonid = 0;
	SDL_ShowMessageBox(&data, &buttonid);
}

void SDLUIInterface::showPlainTextMessageBox(const std::string& title, const std::string& msg)
{
	SDL_MessageBoxData data;
	data.title = title.c_str();
	data.message = msg.c_str();
	int buttonid = 0;
	SDL_ShowMessageBox(&data, &buttonid);
}

void SDLUIInterface::showErrorNotification(const std::string& msg)
{
	conPrint("Error: " + msg); // TEMP
}

void SDLUIInterface::showInfoNotification(const std::string& msg)
{
	conPrint("Info: " + msg); // TEMP
}

void SDLUIInterface::logMessage(const std::string& msg)
{
	conPrint("Log: " + msg); // TEMP
}

void SDLUIInterface::setTextAsNotLoggedIn()
{
	logged_in_username = "";
}

void SDLUIInterface::setTextAsLoggedIn(const std::string& username)
{
	logged_in_username = username;
}

void SDLUIInterface::updateWorldSettingsControlsEditable()
{
}

void SDLUIInterface::updateWorldSettingsUIFromWorldSettings()
{
}

bool SDLUIInterface::diagnosticsVisible()
{
	return false;
}

bool SDLUIInterface::showObAABBsEnabled()
{
	return false;
}

bool SDLUIInterface::showPhysicsObOwnershipEnabled()
{
	return false;
}

bool SDLUIInterface::showVehiclePhysicsVisEnabled()
{
	return false;
}

void SDLUIInterface::writeTransformMembersToObject(WorldObject& ob)
{
}

void SDLUIInterface::objectLastModifiedUpdated(const WorldObject& ob)
{
}

void SDLUIInterface::objectModelURLUpdated(const WorldObject& ob)
{
}

void SDLUIInterface::objectLightmapURLUpdated(const WorldObject& ob)
{
}

void SDLUIInterface::showEditorDockWidget()
{
}

void SDLUIInterface::showParcelEditor()
{
}

void SDLUIInterface::setParcelEditorForParcel(const Parcel& parcel)
{
}

void SDLUIInterface::setParcelEditorEnabled(bool enabled)
{
}

void SDLUIInterface::showObjectEditor()
{
}

void SDLUIInterface::setObjectEditorEnabled(bool enabled)
{
}

void SDLUIInterface::setObjectEditorControlsEditable(bool editable)
{
}

void SDLUIInterface::setObjectEditorFromOb(const WorldObject& ob, int selected_mat_index, bool ob_in_editing_users_world)
{
}

int SDLUIInterface::getSelectedMatIndex()
{
	return 0;
}

void SDLUIInterface::objectEditorToObject(WorldObject& ob)
{
}

void SDLUIInterface::objectEditorObjectPickedUp()
{
}

void SDLUIInterface::objectEditorObjectDropped()
{
}

bool SDLUIInterface::snapToGridCheckBoxChecked()
{
	return false;
}

double SDLUIInterface::gridSpacing()
{
	return 1.0;
}

bool SDLUIInterface::posAndRot3DControlsEnabled()
{
	return true;
}

void SDLUIInterface::setUIForSelectedObject()
{
}

void SDLUIInterface::startObEditorTimerIfNotActive()
{
}

void SDLUIInterface::startLightmapFlagTimer()
{
}

void SDLUIInterface::setCamRotationOnMouseMoveEnabled(bool enabled)
{

}

bool SDLUIInterface::isCursorHidden()
{
	return false;
}

void SDLUIInterface::hideCursor()
{
}

void SDLUIInterface::setKeyboardCameraMoveEnabled(bool enabled)
{
}

bool SDLUIInterface::isKeyboardCameraMoveEnabled()
{
	return true;
}

bool SDLUIInterface::hasFocus()
{
	return (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0; // NOTE: maybe want SDL_WINDOW_MOUSE_FOCUS?
}

void SDLUIInterface::setHelpInfoLabelToDefaultText()
{
}

void SDLUIInterface::setHelpInfoLabel(const std::string& text)
{
}

void SDLUIInterface::toggleFlyMode()
{
	gui_client->player_physics.setFlyModeEnabled(!gui_client->player_physics.flyModeEnabled());
}

void SDLUIInterface::enableThirdPersonCamera()
{
	gui_client->thirdPersonCameraToggled(true);
}

void SDLUIInterface::toggleThirdPersonCameraMode()
{
	gui_client->thirdPersonCameraToggled(!gui_client->cam_controller.thirdPersonEnabled());
}

void SDLUIInterface::enableThirdPersonCameraIfNotAlreadyEnabled()
{
	if(!gui_client->cam_controller.thirdPersonEnabled())
		gui_client->thirdPersonCameraToggled(true);
}

void SDLUIInterface::enableFirstPersonCamera()
{
	gui_client->thirdPersonCameraToggled(false);
}

void SDLUIInterface::openURL(const std::string& URL)
{
}

Vec2i SDLUIInterface::getMouseCursorWidgetPos() // Get mouse cursor position, relative to gl widget.
{
	Vec2i p(0, 0);
	SDL_GetMouseState(&p.x, &p.y);
	return p;
}

std::string SDLUIInterface::getUsernameForDomain(const std::string& domain)
{
	return std::string();
}

std::string SDLUIInterface::getDecryptedPasswordForDomain(const std::string& domain)
{
	return std::string();
}

bool SDLUIInterface::inScreenshotTakingMode()
{
	return false;
}

void SDLUIInterface::setGLWidgetContextAsCurrent()
{
	if(SDL_GL_MakeCurrent(window, gl_context) != 0)
		conPrint("SDL_GL_MakeCurrent failed.");
}

Vec2i SDLUIInterface::getGlWidgetPosInGlobalSpace()
{
	return Vec2i(0, 0);
}

void SDLUIInterface::webViewDataLinkHovered(const std::string& text)
{
}
