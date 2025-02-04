/**
 * rocky c++
 * Copyright 2023 Pelican Mapping
 * MIT License
 */
#pragma once

#include <rocky_vsg/Common.h>
#include <rocky_vsg/engine/ViewLocal.h>
#include <rocky/GeoPoint.h>
#include <vsg/nodes/CullGroup.h>
#include <vsg/nodes/Transform.h>

namespace ROCKY_NAMESPACE
{
    template<class T>
    struct PositionedObjectAdapter : public PositionedObject
    {
        vsg::ref_ptr<T> object;
        virtual const GeoPoint& objectPosition() const {
            return object->position;
        }
        static std::shared_ptr<PositionedObjectAdapter<T>> create(vsg::ref_ptr<T> object_) {
            auto r = std::make_shared< PositionedObjectAdapter<T>>();
            r->object = object_;
            return r;
        }
    };
    /**
     * Transform node that accepts geospatial coordinates and creates
     * a local ENU (X=east, Y=north, Z=up) coordinate frame for its children
     * that is tangent to the earth at the transform's geo position.
     */
    class ROCKY_VSG_EXPORT GeoTransform :
        public vsg::Inherit<vsg::Group, GeoTransform>,
        PositionedObject
    {
    public:
        GeoPoint position;

        //! Sphere for horizon culling
        vsg::dsphere bound = { };

        //! whether horizon culling is active
        bool horizonCulling = true;

    public:
        //! Construct an invalid geotransform
        GeoTransform();

        //! Call this is you change position directly.
        void dirty();

        //! Same as changing position and calling dirty().
        void setPosition(const GeoPoint& p);

    public: // PositionedObject interface

        const GeoPoint& objectPosition() const override {
            return position;
        }

    public:

        GeoTransform(const GeoTransform& rhs) = delete;

        void accept(vsg::RecordTraversal&) const override;

        bool push(vsg::RecordTraversal&, const vsg::dmat4& m) const;

        void pop(vsg::RecordTraversal&) const;

    protected:


        struct Data {
            bool dirty = true;
            GeoPoint worldPos;
            vsg::dmat4 matrix;
            vsg::dmat4 local_matrix;
        };
        util::ViewLocal<Data> _viewlocal;

    };
} // namespace
