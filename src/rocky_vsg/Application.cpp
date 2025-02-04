/**
 * rocky c++
 * Copyright 2023 Pelican Mapping
 * MIT License
 */
#include "Application.h"
#include "MapNode.h"
#include "MapManipulator.h"
#include "SkyNode.h"
#include "json.h"
#include "engine/MeshSystem.h"
#include "engine/LineSystem.h"
#include "engine/IconSystem.h"
#include "engine/LabelSystem.h"

#include <rocky/contrib/EarthFileImporter.h>

#include <vsg/app/CloseHandler.h>
#include <vsg/app/View.h>
#include <vsg/app/RenderGraph.h>
#include <vsg/utils/CommandLine.h>
#include <vsg/utils/ComputeBounds.h>
#include <vsg/vk/State.h>
#include <vsg/io/read.h>
#include <vsg/text/Font.h>
#include <vsg/nodes/DepthSorted.h>

#include <vector>

using namespace ROCKY_NAMESPACE;

namespace
{
    // Call this when adding a new rendergraph to the scene.
    void activateRenderGraph(
        vsg::ref_ptr<vsg::RenderGraph> renderGraph,
        vsg::ref_ptr<vsg::Window> window,
        vsg::ref_ptr<vsg::Viewer> viewer)
    {
        vsg::ref_ptr<vsg::View> view;

        if (!renderGraph->children.empty())
            view = renderGraph->children[0].cast<vsg::View>();

        if (view)
        {
            // add this rendergraph's view to the viewer's compile manager.
            viewer->compileManager->add(*window, view);

            // Compile the new render pass for this view.
            // The lambda idiom is taken from vsgexamples/dynamicviews
            auto result = viewer->compileManager->compile(renderGraph, [&view](vsg::Context& context)
                {
                    return context.view == view.get();
                });

            // if something was compiled, we need to update the viewer:
            if (result.requiresViewerUpdate())
            {
                vsg::updateViewer(*viewer, result);
            }
        }
    }
}

Application::Application(int& argc, char** argv) :
    instance()
{
    vsg::CommandLine commandLine(&argc, argv);

    commandLine.read(instance._impl->runtime.readerWriterOptions);
    _debuglayer = commandLine.read({ "--debug" });
    _apilayer = commandLine.read({ "--api" });
    _vsync = !commandLine.read({ "--novsync" });
    //_multithreaded = commandLine.read({ "--mt" });

    viewer = vsg::Viewer::create();

    root = vsg::Group::create();

    mainScene = vsg::Group::create();

    root->addChild(mainScene);

    mapNode = rocky::MapNode::create(instance);

    // the sun
    if (commandLine.read({ "--sky" }))
    {
        skyNode = rocky::SkyNode::create(instance);
        mainScene->addChild(skyNode);
    }

    mapNode->terrainSettings().concurrency = 6u;
    mapNode->terrainSettings().skirtRatio = 0.025f;
    mapNode->terrainSettings().minLevelOfDetail = 1;
    mapNode->terrainSettings().screenSpaceError = 135.0f;

    // wireframe overlay
    if (commandLine.read({ "--wire" }))
        instance.runtime().shaderCompileSettings->defines.insert("RK_WIREFRAME_OVERLAY");

    // a node to render the map/terrain
    mainScene->addChild(mapNode);

    // Set up the runtime context with everything we need.
    instance.runtime().viewer = viewer;
    instance.runtime().sharedObjects = vsg::SharedObjects::create();

    // TODO:
    // The SkyNode does this, but then it's awkward to add a SkyNode at runtime
    // because various other shaders depend on the define to activate lighting,
    // and those will have to be recompiled.
    // So instead we will just activate the lighting globally and rely on the 
    // light counts in the shader. Is this ok?
    instance.runtime().shaderCompileSettings->defines.insert("RK_LIGHTING");

    // read map from file:
    std::string infile; 
    if (commandLine.read({ "--map" }, infile))
    {
        JSON json;
        if (rocky::util::readFromFile(json, infile))
        {
            mapNode->map->from_json(json);
        }
        else
        {
            Log()->warn("Failed to read map from \"" + infile + "\"");
        }
    }

    // or read map from earth file:
    else if (commandLine.read({ "--earthfile" }, infile))
    {
        std::string msg;
        EarthFileImporter importer;
        auto result = importer.read(infile, instance.ioOptions());
        if (result.status.ok())
        {
            auto count = mapNode->map->layers().size();
            mapNode->map->from_json(result.value);
            if (count == mapNode->map->layers().size())
                msg = "Unable to import any layers from the earth file";

            Log()->warn(json_pretty(result.value));
        }
        else
        {
            msg = "Failed to read earth file - " + result.status.message;
        }
        if (!msg.empty())
        {
            Log()->warn(msg);
        }
    }

    // install the ECS systems that will render components.
    ecs.systems.emplace_back(std::make_shared<MeshSystem>(entities));
    ecs.systems.emplace_back(std::make_shared<LineSystem>(entities));
    ecs.systems.emplace_back(std::make_shared<NodeSystem>(entities));
    ecs.systems.emplace_back(std::make_shared<IconSystem>(entities));
    ecs.systems.emplace_back(std::make_shared<LabelSystem>(entities));

    // install other ECS systems.
    ecs.systems.emplace_back(std::make_shared<EntityMotionSystem>(entities));

    // make a scene graph and connect all the renderer systems to it.
    // This way they will all receive the typical VSG traversals (accept, record, compile, etc.)
    ecs_node = ECS::VSG_SystemsGroup::create();
    ecs_node->connect(ecs);

    mainScene->addChild(ecs_node);
}

Application::~Application()
{
    entities.clear();
}

namespace
{
    // https://github.com/KhronosGroup/Vulkan-Samples/tree/main/samples/extensions/debug_utils
    VKAPI_ATTR VkBool32 VKAPI_CALL debug_utils_messenger_callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
        VkDebugUtilsMessageTypeFlagsEXT message_type,
        const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
        void* user_data)
    {
        if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        {
            Log()->warn("\n" + std::string(callback_data->pMessage));
        }
        else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        {
            Log()->warn("\n" + std::string(callback_data->pMessage));
        }
        return VK_FALSE;
    }
}

util::Future<vsg::ref_ptr<vsg::Window>>
Application::addWindow(vsg::ref_ptr<vsg::WindowTraits> traits)
{
    ROCKY_SOFT_ASSERT_AND_RETURN(traits, { });

    util::Future<vsg::ref_ptr<vsg::Window>> future_window;

    auto add_window = [=]() mutable
    {
        // wait until the device is idle to avoid changing state while it's being used.
        viewer->deviceWaitIdle();

        auto result = future_window;

        //viewer->stopThreading();

        traits->debugLayer = _debuglayer;
        traits->apiDumpLayer = _apilayer;
        if (!_vsync)
            traits->swapchainPreferences.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;

        // This will install the debug messaging callback so we can capture validation errors
        traits->instanceExtensionNames.push_back("VK_EXT_debug_utils");

        // This is required to use the NVIDIA barycentric extension without validation errors
        if (!traits->deviceFeatures)
            traits->deviceFeatures = vsg::DeviceFeatures::create();
        traits->deviceExtensionNames.push_back(VK_NV_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
        auto& bary = traits->deviceFeatures->get<VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR>();
        bary.fragmentShaderBarycentric = true;

        if (viewer->windows().size() > 0)
        {
            traits->device = viewer->windows().front()->getDevice();
        }

        auto window = vsg::Window::create(traits);

        // Each window gets its own CommandGraph. We will store it here and then
        // set it up later when the frame loop starts.
        auto commandgraph = vsg::CommandGraph::create(window);
        _commandGraphByWindow[window] = commandgraph;

        // main camera
        double nearFarRatio = 0.00001;
        double R = mapNode->mapSRS().ellipsoid().semiMajorAxis();
        double ar = (double)traits->width / (double)traits->height;

        auto camera = vsg::Camera::create(
            vsg::Perspective::create(30.0, ar, R * nearFarRatio, R * 20.0),
            vsg::LookAt::create(),
            vsg::ViewportState::create(0, 0, traits->width, traits->height));

        auto view = vsg::View::create(camera, mainScene);

        // add the new view to the window:
        if (_viewerRealized)
            addViewAfterViewerIsRealized(window, view, {}, {});
        else
            addView(window, view, {});

        // Tell Rocky it needs to mutex-protect the terrain engine
        // now that we have more than one window.
        mapNode->terrainSettings().supportMultiThreadedRecord = true;

        // add the new window to our viewer
        viewer->addWindow(window);

        // install a manipulator for the new view:
        addManipulator(window, view);

        result.resolve(window);

        //if (_multithreaded)
        //{
        //    viewer->setupThreading();
        //}

        if (_viewerRealized)
        {
            _viewerDirty = true;
        }

        if (_debuglayer)
        {
            VkDebugUtilsMessengerCreateInfoEXT debug_utils_create_info = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            debug_utils_create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            debug_utils_create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
            debug_utils_create_info.pfnUserCallback = debug_utils_messenger_callback;

            static VkDebugUtilsMessengerEXT debug_utils_messenger;

            auto vki = window->getDevice()->getInstance();

            using PFN_vkCreateDebugUtilsMessengerEXT = VkResult(VKAPI_PTR*)(VkInstance, const VkDebugUtilsMessengerCreateInfoEXT*, const VkAllocationCallbacks*, VkDebugUtilsMessengerEXT*);
            PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT = nullptr;
            if (vki->getProcAddr(vkCreateDebugUtilsMessengerEXT, "vkCreateDebugUtilsMessenger", "vkCreateDebugUtilsMessengerEXT"))
                vkCreateDebugUtilsMessengerEXT(vki->vk(), &debug_utils_create_info, nullptr, &debug_utils_messenger);
        }
    };

    if (_viewerRealized)
        instance.runtime().runDuringUpdate(add_window);
    else
        add_window();

    return future_window;
}

util::Future<vsg::ref_ptr<vsg::View>>
Application::addView(
    vsg::ref_ptr<vsg::Window> window,
    vsg::ref_ptr<vsg::View> view,
    std::function<void(vsg::CommandGraph*)> on_create)
{
    ROCKY_SOFT_ASSERT_AND_RETURN(window != nullptr, {});
    ROCKY_SOFT_ASSERT_AND_RETURN(view != nullptr, {});
    ROCKY_SOFT_ASSERT_AND_RETURN(view->camera != nullptr, {});

    if (_viewerRealized)
    {
        util::Future<vsg::ref_ptr<vsg::View>> result;

        instance.runtime().runDuringUpdate([=]() {
            addViewAfterViewerIsRealized(window, view, on_create, result);
            });

        return result;
    }

    else
    {
        // use this before realization:
        vsg::ref_ptr<vsg::CommandGraph> commandgraph;

        auto iter = _commandGraphByWindow.find(window);
        if (iter != _commandGraphByWindow.end())
        {
            commandgraph = iter->second;

            if (view->children.empty())
            {
                view->addChild(root);
            }

            auto rendergraph = vsg::RenderGraph::create(window, view);
            rendergraph->setClearValues({ {0.1f, 0.12f, 0.15f, 1.0f} });
            commandgraph->addChild(rendergraph);

            auto& viewdata = _viewData[view];
            viewdata.parentRenderGraph = rendergraph;

            displayConfiguration.windows[window].emplace_back(view);
        }

        // return a resolved future since we are immediately good to go
        util::Future<vsg::ref_ptr<vsg::View>> result;
        result.resolve(view);

        if (on_create)
            on_create(commandgraph.get());

        return result;
    }
}

vsg::ref_ptr<vsg::CommandGraph>
Application::getCommandGraph(vsg::ref_ptr<vsg::Window> window)
{
    auto iter = _commandGraphByWindow.find(window);
    if (iter != _commandGraphByWindow.end())
        return iter->second;
    else
        return {};
}

vsg::ref_ptr<vsg::Window>
Application::getWindow(vsg::ref_ptr<vsg::View> view)
{
    for (auto iter : displayConfiguration.windows)
    {
        for (auto& a_view : iter.second)
        {
            if (a_view == view)
            {
                return iter.first;
                break;
            }
        }
    }
    return {};
}

void
Application::addViewAfterViewerIsRealized(
    vsg::ref_ptr<vsg::Window> window,
    vsg::ref_ptr<vsg::View> view,
    std::function<void(vsg::CommandGraph*)> on_create,
    util::Future<vsg::ref_ptr<vsg::View>> result)
{
    // wait until the device is idle to avoid changing state while it's being used.
    viewer->deviceWaitIdle();

    // attach our scene to the new view:
    if (view->children.empty())
    {
        view->addChild(root);
    }

    // find the command graph for this window:
    auto commandgraph = getCommandGraph(window);
    if (commandgraph)
    {
        // new view needs a new rendergraph:
        auto rendergraph = vsg::RenderGraph::create(window, view);
        rendergraph->setClearValues({ {0.1f, 0.12f, 0.15f, 1.0f} });
        commandgraph->addChild(rendergraph);

        activateRenderGraph(rendergraph, window, viewer);

        // remember so we can remove it later
        auto& viewdata = _viewData[view];
        viewdata.parentRenderGraph = rendergraph;
        displayConfiguration.windows[window].emplace_back(view);

        // Add a manipulator - we might not do this by default - check back.
        addManipulator(window, view);
    }

    if (on_create)
        on_create(commandgraph.get());

    // report that we are ready to rock
    result.resolve(view);
}

void
Application::removeView(vsg::ref_ptr<vsg::View> view)
{
    ROCKY_SOFT_ASSERT_AND_RETURN(view != nullptr, void());

    auto remove = [=]()
    {
        // wait until the device is idle to avoid changing state while it's being used.
        viewer->deviceWaitIdle();

        auto window = getWindow(view);
        ROCKY_SOFT_ASSERT_AND_RETURN(window != nullptr, void());

        auto commandgraph = getCommandGraph(window);
        ROCKY_SOFT_ASSERT_AND_RETURN(commandgraph, void());

        // find the rendergraph hosting the view:
        auto vd = _viewData.find(view);
        ROCKY_SOFT_ASSERT_AND_RETURN(vd != _viewData.end(), void());
        auto& rendergraph = vd->second.parentRenderGraph;

        // remove the rendergraph from the command graph.
        auto& rps = commandgraph->children;
        rps.erase(std::remove(rps.begin(), rps.end(), rendergraph), rps.end());

        // remove it from our tracking tables.
        _viewData.erase(view);
        auto& views = displayConfiguration.windows[vsg::observer_ptr<vsg::Window>(window)];
        views.erase(std::remove(views.begin(), views.end(), view), views.end());
    };

    if (_viewerRealized)
        instance.runtime().runDuringUpdate(remove);

    else
        remove();
}

void
Application::refreshView(vsg::ref_ptr<vsg::View> view)
{
    auto refresh = [=]()
    {
        ROCKY_SOFT_ASSERT_AND_RETURN(view, void());

        auto& viewdata = _viewData[view];
        ROCKY_SOFT_ASSERT_AND_RETURN(viewdata.parentRenderGraph, void());

        // wait until the device is idle to avoid changing state while it's being used.
        viewer->deviceWaitIdle();

        auto vp = view->camera->getViewport();
        viewdata.parentRenderGraph->renderArea.offset.x = (std::uint32_t)vp.x;
        viewdata.parentRenderGraph->renderArea.offset.y = (std::uint32_t)vp.y;
        viewdata.parentRenderGraph->renderArea.extent.width = (std::uint32_t)vp.width;
        viewdata.parentRenderGraph->renderArea.extent.height = (std::uint32_t)vp.height;

        // rebuild the graphics pipelines to reflect new camera/view params.
        vsg::UpdateGraphicsPipelines u;
        u.context = vsg::Context::create(viewdata.parentRenderGraph->getRenderPass()->device);
        u.context->renderPass = viewdata.parentRenderGraph->getRenderPass();
        viewdata.parentRenderGraph->accept(u);
    };

    if (_viewerRealized)
        instance.runtime().runDuringUpdate(refresh);
    else
        refresh();
}

void
Application::addPreRenderGraph(vsg::ref_ptr<vsg::Window> window, vsg::ref_ptr<vsg::RenderGraph> renderGraph)
{
    auto func = [=]()
    {
        auto commandGraph = getCommandGraph(window);

        ROCKY_SOFT_ASSERT_AND_RETURN(commandGraph, void());
        ROCKY_SOFT_ASSERT_AND_RETURN(commandGraph->children.size() > 0, void());

        // Insert the pre-render graph into the command graph.
        commandGraph->children.insert(commandGraph->children.begin(), renderGraph);

        // hook it up.
        activateRenderGraph(renderGraph, window, viewer);
    };

    if (_viewerRealized)
        instance.runtime().runDuringUpdate(func);
    else
        func();
}

void
Application::setupViewer(vsg::ref_ptr<vsg::Viewer> viewer)
{
    // Initialize the ECS subsystem:
    ecs_node->initialize(instance.runtime());

    // respond to the X or to hitting ESC
    // TODO: refactor this so it responds to individual windows and not the whole app?
    viewer->addEventHandler(vsg::CloseHandler::create(viewer));

    // This sets up the internal tasks that will, for each command graph, record
    // a scene graph and submit the results to the renderer each frame. Also sets
    // up whatever's necessary to present the resulting swapchain to the device.
    vsg::CommandGraphs commandGraphs;
    for (auto iter : _commandGraphByWindow)
    {
        commandGraphs.push_back(iter.second);
    }

    viewer->assignRecordAndSubmitTaskAndPresentation(commandGraphs);


#if 1
    // Configure a descriptor pool size that's appropriate for terrain
    // https://groups.google.com/g/vsg-users/c/JJQZ-RN7jC0/m/tyX8nT39BAAJ
    // https://www.reddit.com/r/vulkan/comments/8u9zqr/having_trouble_understanding_descriptor_pool/    
    // Since VSG dynamic allocates descriptor pools as it needs them,
    // this is not strictly necessary. But if you know something about the
    // types of descriptor sets you are going to use it will increase 
    // performance (?) to pre-allocate some pools like this.
    // There is a big trade-off since pre-allocating these pools takes up
    // a significant amount of memory.

    auto resourceHints = vsg::ResourceHints::create();

    // max number of descriptor sets per pool, regardless of type:
    resourceHints->numDescriptorSets = 1;

    // max number of descriptor sets of a specific type per pool:
    //resourceHints->descriptorPoolSizes.push_back(
    //    VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 });

    viewer->compile(resourceHints);

#else
    viewer->compile();
#endif
}

void
Application::recreateViewer() 
{
    // Makes a new viewer, copying settings from the old viewer.
    vsg::EventHandlers handlers = viewer->getEventHandlers();

    // before we destroy it,
    // wait until the device is idle to avoid changing state while it's being used.
    viewer->deviceWaitIdle();

    viewer = vsg::Viewer::create();
    
    for (auto i : displayConfiguration.windows)
        viewer->addWindow(i.first);

    for (auto& j : handlers)
        viewer->addEventHandler(j);

    setupViewer(viewer);
}

void
Application::realize()
{
    if (!_viewerRealized)
    {
        // Make a window if the user didn't.
        if (viewer->windows().empty())
        {
            addWindow(vsg::WindowTraits::create(1920, 1080, "Main Window"));
        }

        setupViewer(viewer);

        // mark the viewer ready so that subsequent changes will know to
        // use an asynchronous path.
        _viewerRealized = true;
    }
}

int
Application::run()
{
    // The main frame loop
    while (frame() == true);    
    return 0;
}

bool
Application::frame()
{
    ROCKY_PROFILE_FUNCTION();

    if (!_viewerRealized)
        realize();

    auto t_start = std::chrono::steady_clock::now();

    if (!viewer->advanceToNextFrame())
        return false;

    auto t_update = std::chrono::steady_clock::now();

    // rocky map update pass - management of tiles and paged data
    mapNode->update(viewer->getFrameStamp());

    // ECS updates
    ecs.update(viewer->getFrameStamp()->time);
    ecs_node->update(instance.runtime());

    // User update
    if (updateFunction)
        updateFunction();

    // Event handling happens after updating the scene, otherwise
    // things like tethering to a moving node will be one frame behind
    viewer->handleEvents();

    // run through the viewer's update operations queue; this includes
    // update ops initialized by rocky (e.g. terrain tile merges)
    viewer->update();

    // integrate any compile results that may be pending
    instance.runtime().update();

    if (_viewerDirty)
    {
        _viewerDirty = false;
        recreateViewer();
        return true;
    }

    auto t_record = std::chrono::steady_clock::now();

    viewer->recordAndSubmit();

    auto t_present = std::chrono::steady_clock::now();

    viewer->present();

    auto t_end = std::chrono::steady_clock::now();
    stats.frame = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start);
    stats.events = std::chrono::duration_cast<std::chrono::microseconds>(t_update - t_start);
    stats.update = std::chrono::duration_cast<std::chrono::microseconds>(t_record - t_update);
    stats.record = std::chrono::duration_cast<std::chrono::microseconds>(t_present - t_record);
    stats.present = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_present);

    return viewer->active();
}

void
Application::addManipulator(vsg::ref_ptr<vsg::Window> window, vsg::ref_ptr<vsg::View> view)
{
    auto manip = MapManipulator::create(mapNode, window, view->camera);

    // stow this away in the view object so it's easy to find later.
    view->setObject(MapManipulator::tag, manip);

    // The manipulators (one for each view) need to be in the right order (top to bottom)
    // so that overlapping views don't get mixed up. To accomplish this we'll just
    // remove them all and re-insert them in the new proper order:
    auto& ehs = viewer->getEventHandlers();

    // remove all the MapManipulators using the dumb remove-erase idiom
    ehs.erase(
        std::remove_if(
            ehs.begin(), ehs.end(),
            [](const vsg::ref_ptr<vsg::Visitor>& v) { return dynamic_cast<MapManipulator*>(v.get()); }),
        ehs.end()
    );

    // re-add them in the right order (last to first)
    for (auto& window : displayConfiguration.windows)
    {
        for(auto vi = window.second.rbegin(); vi != window.second.rend(); ++vi)
        {
            auto& view = *vi;
            auto manip = view->getRefObject<MapManipulator>(MapManipulator::tag);
            ehs.push_back(manip);
        }
    }
}

std::string
Application::about() const
{
    std::stringstream buf;
    for (auto& a : instance.about())
        buf << a << std::endl;
    return buf.str();
}
