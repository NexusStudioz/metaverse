#include "ObjectEditor.h"


#include "PlayerPhysics.h"
#include "../dll/include/IndigoMesh.h"
#include "../indigo/TextureServer.h"
#include "../indigo/globals.h"
#include "../graphics/Map2D.h"
#include "../graphics/imformatdecoder.h"
#include "../graphics/ImageMap.h"
#include "../maths/vec3.h"
#include "../maths/GeometrySampling.h"
#include "../utils/Lock.h"
#include "../utils/Mutex.h"
#include "../utils/Clock.h"
#include "../utils/Timer.h"
#include "../utils/Platform.h"
#include "../utils/FileUtils.h"
#include "../utils/Reference.h"
#include "../utils/StringUtils.h"
#include "../utils/CameraController.h"
#include "../utils/TaskManager.h"
#include "../qt/SignalBlocker.h"
#include "../qt/QtUtils.h"
#include <QtGui/QMouseEvent>
#include <set>
#include <stack>
#include <algorithm>


ObjectEditor::ObjectEditor(QWidget *parent)
:	QWidget(parent)
{
	setupUi(this);

	this->scaleXDoubleSpinBox->setMinimum(0.00001);
	this->scaleYDoubleSpinBox->setMinimum(0.00001);
	this->scaleZDoubleSpinBox->setMinimum(0.00001);

	connect(this->matEditor,				SIGNAL(materialChanged()),			this, SIGNAL(objectChanged()));

	connect(this->modelFileSelectWidget,	SIGNAL(filenameChanged(QString&)),	this, SIGNAL(objectChanged()));

	connect(this->scaleXDoubleSpinBox,		SIGNAL(valueChanged(double)),		this, SIGNAL(objectChanged()));
	connect(this->scaleYDoubleSpinBox,		SIGNAL(valueChanged(double)),		this, SIGNAL(objectChanged()));
	connect(this->scaleZDoubleSpinBox,		SIGNAL(valueChanged(double)),		this, SIGNAL(objectChanged()));
	
	connect(this->rotAxisXDoubleSpinBox,	SIGNAL(valueChanged(double)),		this, SIGNAL(objectChanged()));
	connect(this->rotAxisYDoubleSpinBox,	SIGNAL(valueChanged(double)),		this, SIGNAL(objectChanged()));
	connect(this->rotAxisZDoubleSpinBox,	SIGNAL(valueChanged(double)),		this, SIGNAL(objectChanged()));
	connect(this->rotAngleDoubleSpinBox,	SIGNAL(valueChanged(double)),		this, SIGNAL(objectChanged()));
	
	//setControlsEnabled(false);
}


ObjectEditor::~ObjectEditor()
{
}


void ObjectEditor::setFromObject(const WorldObject& ob)
{
	this->modelFileSelectWidget->setFilename(QtUtils::toQString(ob.model_url));

	SignalBlocker::setValue(this->scaleXDoubleSpinBox, ob.scale.x);
	SignalBlocker::setValue(this->scaleYDoubleSpinBox, ob.scale.y);
	SignalBlocker::setValue(this->scaleZDoubleSpinBox, ob.scale.z);
	
	SignalBlocker::setValue(this->rotAxisXDoubleSpinBox, ob.axis.x);
	SignalBlocker::setValue(this->rotAxisYDoubleSpinBox, ob.axis.y);
	SignalBlocker::setValue(this->rotAxisZDoubleSpinBox, ob.axis.z);
	SignalBlocker::setValue(this->rotAngleDoubleSpinBox, ob.angle);

	WorldMaterialRef mat_0;
	if(ob.materials.empty())
		mat_0 = new WorldMaterial();
	else
		mat_0 = ob.materials[0];

	this->matEditor->setFromMaterial(*mat_0);
}


void ObjectEditor::toObject(WorldObject& ob_out)
{
	ob_out.model_url = QtUtils::toIndString(this->modelFileSelectWidget->filename());

	ob_out.scale.x = (float)this->scaleXDoubleSpinBox->value();
	ob_out.scale.y = (float)this->scaleYDoubleSpinBox->value();
	ob_out.scale.z = (float)this->scaleZDoubleSpinBox->value();

	ob_out.axis.x = (float)this->rotAxisXDoubleSpinBox->value();
	ob_out.axis.y = (float)this->rotAxisYDoubleSpinBox->value();
	ob_out.axis.z = (float)this->rotAxisZDoubleSpinBox->value();
	ob_out.angle  = (float)this->rotAngleDoubleSpinBox->value();

	if(ob_out.axis.length() < 1.0e-5f)
	{
		ob_out.axis = Vec3f(0,0,1);
		ob_out.angle = 0;
	}

	if(ob_out.materials.empty())
		ob_out.materials.push_back(new WorldMaterial());

	this->matEditor->toMaterial(*ob_out.materials[0]);
}


void ObjectEditor::setControlsEnabled(bool enabled)
{
	this->setEnabled(enabled);
}
