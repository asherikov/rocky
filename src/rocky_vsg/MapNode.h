/**
 * rocky c++
 * Copyright 2023 Pelican Mapping
 * MIT License
 */
#pragma once

#include <rocky_vsg/Common.h>
#include <rocky_vsg/InstanceVSG.h>
#include <rocky_vsg/TerrainSettings.h>
#include <rocky_vsg/engine/TerrainNode.h>
#include <rocky/Map.h>
#include <vsg/nodes/Group.h>
#include <vsg/app/CompileManager.h>

namespace ROCKY_NAMESPACE
{
    class SRS;

    /**
     * VSG Node that renders a map.
     * This node is a "view" component that renders data from a "Map" data model.
     */
    class ROCKY_VSG_EXPORT MapNode : public vsg::Inherit<vsg::Group, MapNode>
    {
    public: // constructors

        //! Creates an empty map node (with a default empty Map)
        MapNode(const InstanceVSG& instance);

        //! Creates a map node that will render the given Map.
        MapNode(shared_ptr<Map> map);

        //! Deserialize a MapNode
        explicit MapNode(const JSON& conf, const InstanceVSG& instance);

    public:

        //! Map backing this map node
        std::shared_ptr<Map> map;

        //! Instance object assocated with this map node
        InstanceVSG instance;

        //! Node rendering the terrain surface
        vsg::ref_ptr<TerrainNode> terrain;

    public:

        //! Screen-space error for geometry level of detail
        void setScreenSpaceError(float sse);
        float screenSpaceError() const;

        //! Spatial Reference System of the underlying map.
        const SRS& mapSRS() const;

        //! Spatial reference system of the rendered map.
        const SRS& worldSRS() const;

        //! Mutable access to the terrain settings
        TerrainSettings& terrainSettings();

        //! Immutable access to the terrain settings
        const TerrainSettings& terrainSettings() const;

        //! Serializes the MapNode
        JSON to_json() const;

        //! Opens the map (installs a terrain engine and initializes all the layers)
        bool open();

        void update(const vsg::FrameStamp*);

    public:

        void accept(vsg::RecordTraversal&) const override;

    private:

        void construct(const JSON&);


        optional<float> _screenSpaceError = 25.0f;
        SRS _worldSRS;
        vsg::ref_ptr<vsg::Group> _layerNodes;
        std::atomic<bool> _readyForUpdate;

        bool _isOpen;
    };
}
