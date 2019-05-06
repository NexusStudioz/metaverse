/*=====================================================================
MaterialBrowser.cpp
-------------------
Copyright Glare Technologies Limited 2019 -
=====================================================================*/
#include "MaterialBrowser.h"


#include <QtWidgets/QPushButton>
#include <QtGui/QOffscreenSurface>
#include <QtGui/QOpenGLFramebufferObject>
#include <QtOpenGL/QGLWidget>
#include <QtGui/QOpenGLContext>
#include <QtGui/QImageWriter>
#include "../qt/FlowLayout.h"
#include "../shared/WorldMaterial.h"
#include "ModelLoading.h"
#include <opengl/OpenGLEngine.h>
#include <graphics/PNGDecoder.h>
#include <graphics/imformatdecoder.h>
#include <qt/QtUtils.h>
#include <FileUtils.h>
#include <StringUtils.h>
#include <ConPrint.h>
#include <IncludeXXHash.h>
#include <Exception.h>
#include "../indigo/TextureServer.h"
#include "../dll/include/IndigoMesh.h"


#define PREVIEW_SIZE 150


MaterialBrowser::MaterialBrowser()
:	fbo(NULL),
	context(NULL),
	offscreen_surface(NULL)
{
}


MaterialBrowser::~MaterialBrowser()
{
}


// Set up offscreen rendering and OpenGL engine
void MaterialBrowser::createOpenGLEngineAndSurface()
{
	QSurfaceFormat format;
	format.setSamples(4); // For MSAA
	format.setColorSpace(QSurfaceFormat::sRGBColorSpace);

	this->context = new QOpenGLContext();
	this->context->setFormat(format);
	this->context->create();
	assert(context->isValid());

	this->offscreen_surface = new QOffscreenSurface();
	this->offscreen_surface->setFormat(context->format());
	this->offscreen_surface->create();
	assert(this->offscreen_surface->isValid());

	context->makeCurrent(this->offscreen_surface);


	OpenGLEngineSettings settings;
	settings.shadow_mapping = true;
	settings.compress_textures = true;
	opengl_engine = new OpenGLEngine(settings);

	opengl_engine->initialise(
		basedir_path + "/data", // data dir (should contain 'shaders' and 'gl_data')
		texture_server_ptr
	);
	if(!opengl_engine->initSucceeded())
	{
		conPrint("opengl_engine init failed: " + opengl_engine->getInitialisationErrorMsg());
	}

	QOpenGLFramebufferObjectFormat fbo_format;
	fbo_format.setSamples(4); // For MSAA
	fbo_format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil); // Seems to be needed for shadow mapping.
	this->fbo = new QOpenGLFramebufferObject(QSize(PREVIEW_SIZE, PREVIEW_SIZE), fbo_format);
	assert(fbo->isValid());

	const float sun_phi = 1.f;
	const float sun_theta = Maths::pi<float>() / 4;
	opengl_engine->setSunDir(normalise(Vec4f(std::cos(sun_phi) * sin(sun_theta), std::sin(sun_phi) * sun_theta, cos(sun_theta), 0)));

	// Add env mat
	{
		OpenGLMaterial env_mat;
		env_mat.albedo_tex_path = basedir_path + "/resources/sky_no_sun.exr";
		env_mat.tex_matrix = Matrix2f(-1 / Maths::get2Pi<float>(), 0, 0, 1 / Maths::pi<float>());
		opengl_engine->setEnvMat(env_mat);
	}

	// Load a ground plane into the GL engine
	{
		const float W = 200;

		GLObjectRef ob = new GLObject();
		ob->materials.resize(1);
		ob->materials[0].albedo_rgb = Colour3f(0.9f);
		ob->materials[0].albedo_tex_path = "resources/obstacle.png";
		ob->materials[0].roughness = 0.8f;
		ob->materials[0].fresnel_scale = 0.5f;
		ob->materials[0].tex_matrix = Matrix2f(W, 0, 0, W);

		ob->ob_to_world_matrix = Matrix4f::scaleMatrix(W, W, 1) * Matrix4f::translationMatrix(-0.5f, -0.5f, 0);
		ob->mesh_data = opengl_engine->makeUnitQuadMesh();

		opengl_engine->addObject(ob);
	}

	glViewport(0, 0, PREVIEW_SIZE, PREVIEW_SIZE);
	opengl_engine->viewportChanged(PREVIEW_SIZE, PREVIEW_SIZE);

	const Matrix4f world_to_camera_space_matrix =  Matrix4f::rotationAroundXAxis(0.5f) * Matrix4f::translationMatrix(0, 0.8, -0.6) * Matrix4f::rotationAroundZAxis(2.5);

	const float sensor_width = 0.035f;
	const float lens_sensor_dist = 0.03f;
	const float render_aspect_ratio = 1.0;

	opengl_engine->setViewportAspectRatio(1.0, PREVIEW_SIZE, PREVIEW_SIZE);
	opengl_engine->setMaxDrawDistance(100.f);
	opengl_engine->setPerspectiveCameraTransform(world_to_camera_space_matrix, sensor_width, lens_sensor_dist, render_aspect_ratio, /*lens shift up=*/0.f, /*lens shift right=*/0.f);
}


void MaterialBrowser::init(QWidget* parent, const std::string& basedir_path_, const std::string& appdata_path_, TextureServer* texture_server_ptr_)
{
	basedir_path = basedir_path_;
	appdata_path = appdata_path_;
	texture_server_ptr = texture_server_ptr_;

	setupUi(this);

	flow_layout = new FlowLayout(this);

	// Scan for all materials on disk, make preview buttons for them.
	try
	{
		const std::vector<std::string> filepaths = FileUtils::getFilesInDirWithExtensionFullPaths(basedir_path + "/resources/materials", "submat");

		for(size_t i=0; i<filepaths.size(); ++i)
		{
			const std::string EPOCH_STRING = "_1"; // Can change to invalidate cache.
			const std::string cache_key_input = FileUtils::getFilename(filepaths[i]) + EPOCH_STRING;
			const uint64 cache_hashkey = XXH64(cache_key_input.data(), cache_key_input.size(), 1);
			const uint64 dir_bits = cache_hashkey >> 58; // 6 bits for the dirs => 64 subdirs in program_cache.
			const std::string dir = ::toHexString(dir_bits);
			const std::string cachefile_path = appdata_path + "/material_cache/" + dir + "/" + toHexString(cache_hashkey) + ".jpg";

			QImage image;
			if(FileUtils::fileExists(cachefile_path))
			{
				image.load(QtUtils::toQString(cachefile_path));
			}

			if(image.isNull()) // If image on disk was not found, or failed to load:
			{
				if(opengl_engine.isNull())
					createOpenGLEngineAndSurface();

				assert(opengl_engine.nonNull());

				// Render the preview image

				// Add voxel
				GLObjectRef voxel_ob = new GLObject();
				{
					voxel_ob->materials.resize(1);

					WorldMaterialRef mat = WorldMaterial::loadFromXMLOnDisk(filepaths[i]);

					ModelLoading::setGLMaterialFromWorldMaterialWithLocalPaths(*mat, voxel_ob->materials[0]);

					const float voxel_w = 0.5f;
					voxel_ob->ob_to_world_matrix = Matrix4f::translationMatrix(0, 0, voxel_w/2) * Matrix4f::rotationAroundZAxis(Maths::pi_4<float>()) *
						Matrix4f::uniformScaleMatrix(voxel_w) * Matrix4f::translationMatrix(-0.5f, -0.5f, -0.5f);
					voxel_ob->mesh_data = opengl_engine->getCubeMeshData();

					opengl_engine->addObject(voxel_ob);
				}

				opengl_engine->setTargetFrameBuffer(this->fbo->handle());
				opengl_engine->draw();

				glFinish();

				opengl_engine->removeObject(voxel_ob);

				image = fbo->toImage();

				// Save image to disk.
				try
				{
					FileUtils::createDirsForPath(cachefile_path);
					QImageWriter writer(QtUtils::toQString(cachefile_path));
					writer.setQuality(95);
					const bool saved = writer.write(image);
					if(!saved)
						conPrint("Warning: failed to save cached material preview image to '" + cachefile_path + "'.");
				}
				catch(FileUtils::FileUtilsExcep& e)
				{
					conPrint("Warning: failed to save cached material preview image to '" + cachefile_path + "': " + e.what());
				}
			}

			QPushButton* button = new QPushButton();

			button->setFixedWidth(PREVIEW_SIZE);
			button->setFixedHeight(PREVIEW_SIZE);
			button->setIconSize(QSize(PREVIEW_SIZE, PREVIEW_SIZE));

			button->setIcon(QPixmap::fromImage(image));

			flow_layout->addWidget(button);

			connect(button, SIGNAL(clicked()), this, SLOT(buttonClicked()));

			browser_buttons.push_back(button);
			mat_paths.push_back(filepaths[i]);
		}
	}
	catch(FileUtils::FileUtilsExcep& e)
	{
		conPrint("Error: " + e.what());
	}
	catch(Indigo::Exception& e)
	{
		conPrint("Error: " + e.what());
	}

	// Free OpenGL engine, offscreen surfaces etc. if they were allocated.
	delete this->fbo;
	opengl_engine = NULL;
	delete this->offscreen_surface;
	delete this->context;
}


void MaterialBrowser::buttonClicked()
{
	assert(mat_paths.size() == browser_buttons.size());

	QPushButton* button = static_cast<QPushButton*>(QObject::sender());
	for(size_t z=0; z<browser_buttons.size(); ++z)
		if(browser_buttons[z] == button)
		{
			emit materialSelected(mat_paths[z]);
		}
}