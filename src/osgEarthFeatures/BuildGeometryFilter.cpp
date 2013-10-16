/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2013 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include <osgEarthFeatures/BuildGeometryFilter>
#include <osgEarthFeatures/FeatureSourceIndexNode>
#include <osgEarthFeatures/PolygonizeLines>
#include <osgEarthSymbology/TextSymbol>
#include <osgEarthSymbology/PointSymbol>
#include <osgEarthSymbology/LineSymbol>
#include <osgEarthSymbology/PolygonSymbol>
#include <osgEarthSymbology/MeshSubdivider>
#include <osgEarthSymbology/MeshConsolidator>
#include <osgEarth/Utils>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/LineWidth>
#include <osg/LineStipple>
#include <osg/Point>
#include <osg/Depth>
#include <osg/PolygonOffset>
#include <osg/MatrixTransform>
#include <osgText/Text>
#include <osgUtil/Tessellator>
#include <osgUtil/Optimizer>
#include <osgUtil/SmoothingVisitor>
#include <osgDB/WriteFile>
#include <osg/Version>

#define LC "[BuildGeometryFilter] "

using namespace osgEarth;
using namespace osgEarth::Features;
using namespace osgEarth::Symbology;

#define USE_SINGLE_COLOR 0


BuildGeometryFilter::BuildGeometryFilter( const Style& style ) :
_style        ( style ),
_maxAngle_deg ( 1.0 ),
_geoInterp    ( GEOINTERP_RHUMB_LINE ),
_mergeGeometry( false ),
_useVertexBufferObjects( true )
{
    reset();
}

void
BuildGeometryFilter::reset()
{
    _result = 0L;
    _geode = new osg::Geode();
    _hasLines = false;
    _hasPoints = false;
    _hasPolygons = false;
}

bool
BuildGeometryFilter::process( FeatureList& features, const FilterContext& context )
{
    bool makeECEF = false;
    const SpatialReference* featureSRS = 0L;
    const SpatialReference* mapSRS = 0L;

    if ( context.isGeoreferenced() )
    {
        makeECEF   = context.getSession()->getMapInfo().isGeocentric();
        featureSRS = context.extent()->getSRS();
        mapSRS     = context.getSession()->getMapInfo().getProfile()->getSRS();
    }

    for( FeatureList::iterator f = features.begin(); f != features.end(); ++f )
    {
        Feature* input = f->get();

        GeometryIterator parts( input->getGeometry(), false );
        while( parts.hasMore() )
        {
            Geometry* part = parts.next();

            // skip empty geometry
            if ( part->size() == 0 )
                continue;

            const Style& myStyle = input->style().isSet() ? *input->style() : _style;

            bool  setLinePropsHere   = input->style().isSet(); // otherwise it will be set globally, we assume
            float width              = 1.0f;
            bool  hasPolyOutline     = false;

            const PointSymbol*   pointSymbol = myStyle.get<PointSymbol>();
            const LineSymbol*    lineSymbol  = myStyle.get<LineSymbol>();
            const PolygonSymbol* polySymbol  = myStyle.get<PolygonSymbol>();

            // resolve the geometry type from the component type and the symbology:
            Geometry::Type renderType = Geometry::TYPE_UNKNOWN;

            // First priority is the symbol with a compatible part type.
            if (polySymbol != 0L && 
                part->getType() != Geometry::TYPE_POINTSET && 
                part->getTotalPointCount() >= 3)
            {
                renderType = Geometry::TYPE_POLYGON;
            }
            else if (lineSymbol != 0L)
            {
                if ( part->getType() == Geometry::TYPE_POLYGON )
                    renderType = Geometry::TYPE_RING;
                else
                    renderType = part->getType();
            }
            else if (pointSymbol != 0L)
            {
                renderType = Geometry::TYPE_POINTSET;
            }

            // fall back on just using the geometry type.
            else
            {
                renderType = part->getType();
            }

            // validate the geometry:
            if ( renderType == Geometry::TYPE_POLYGON && part->size() < 3 )
                continue;
            else if ( (renderType == Geometry::TYPE_LINESTRING || renderType == Geometry::TYPE_RING) && part->size() < 2 )
                continue;

            // resolve the color:
            osg::Vec4f primaryColor =
                polySymbol ? osg::Vec4f(polySymbol->fill()->color()) :
                lineSymbol ? osg::Vec4f(lineSymbol->stroke()->color()) :
                pointSymbol ? osg::Vec4f(pointSymbol->fill()->color()) :
                osg::Vec4f(1,1,1,1);
            
            osg::ref_ptr<osg::Geometry> osgGeom = new osg::Geometry();
            osgGeom->setUseVertexBufferObjects( _useVertexBufferObjects.value() );

            if ( _featureNameExpr.isSet() )
            {
                const std::string& name = input->eval( _featureNameExpr.mutable_value(), &context );
                osgGeom->setName( name );
            }

            // build the geometry:
            osg::Vec3Array* allPoints = 0L;

            if ( renderType == Geometry::TYPE_POLYGON )
            {
                buildPolygon(part, featureSRS, mapSRS, makeECEF, true, osgGeom);
                //allPoints = static_cast<osg::Vec3Array*>( osgGeom->getVertexArray() );
            }
            else
            {
                // line or point geometry
                GLenum primMode = 
                    renderType == Geometry::TYPE_LINESTRING ? GL_LINE_STRIP :
                    renderType == Geometry::TYPE_RING       ? GL_LINE_LOOP :
                    GL_POINTS;

                allPoints = new osg::Vec3Array();
                transformAndLocalize( part->asVector(), featureSRS, allPoints, mapSRS, _world2local, makeECEF );

                if ( lineSymbol && lineSymbol->stroke()->widthUnits() != Units::PIXELS )
                {
                    PolygonizeLinesOperator plo( *lineSymbol->stroke() );
                    osgGeom = plo( allPoints, 0L );
                }
                else
                {
                    osgGeom->addPrimitiveSet( new osg::DrawArrays( primMode, 0, allPoints->getNumElements() ) );
                    osgGeom->setVertexArray( allPoints );

                    if ( lineSymbol )
                        applyLineSymbology( osgGeom->getOrCreateStateSet(), lineSymbol );
                    if ( pointSymbol )
                        applyPointSymbology( osgGeom->getOrCreateStateSet(), pointSymbol );
                }

                if ( primMode == GL_POINTS && allPoints->size() == 1 )
                {
                    const osg::Vec3d& center = (*allPoints)[0];
                    osgGeom->setInitialBound( osg::BoundingBox(center-osg::Vec3(.5,.5,.5), center+osg::Vec3(.5,.5,.5)) );
                }
            }

            allPoints = static_cast<osg::Vec3Array*>(osgGeom->getVertexArray());
            if (allPoints->getVertexBufferObject())
                allPoints->getVertexBufferObject()->setUsage(GL_STATIC_DRAW_ARB);
            
            // subdivide the mesh if necessary to conform to an ECEF globe:
            if ( makeECEF && renderType != Geometry::TYPE_POINTSET )
            {
                // check for explicit tessellation disable:
                const LineSymbol* line = _style.get<LineSymbol>();
                bool disableTess = line && line->tessellation().isSetTo(0);

                if ( makeECEF && !disableTess )
                {                    
                    double threshold = osg::DegreesToRadians( *_maxAngle_deg );
                    OE_DEBUG << "Running mesh subdivider with threshold " << *_maxAngle_deg << std::endl;

                    MeshSubdivider ms( _world2local, _local2world );
                    //ms.setMaxElementsPerEBO( INT_MAX );
                    if ( input->geoInterp().isSet() )
                        ms.run( *osgGeom, threshold, *input->geoInterp() );
                    else
                        ms.run( *osgGeom, threshold, *_geoInterp );
                }
            }

            // assign the primary color:
#if USE_SINGLE_COLOR            
            osg::Vec4Array* colors = new osg::Vec4Array( 1 );
            (*colors)[0] = primaryColor;            
            osgGeom->setColorArray( colors );
            osgGeom->setColorBinding( osg::Geometry::BIND_OVERALL );
#else

            osg::Vec4Array* colors = new osg::Vec4Array;
            colors->assign( osgGeom->getVertexArray()->getNumElements(), primaryColor );
            osgGeom->setColorArray( colors );
            osgGeom->setColorBinding( osg::Geometry::BIND_PER_VERTEX );
#endif

            _geode->addDrawable( osgGeom );

            // record the geometry's primitive set(s) in the index:
            if ( context.featureIndex() )
                context.featureIndex()->tagPrimitiveSets( osgGeom, input );

#if 0
            // build secondary geometry, if necessary (polygon outlines)
            if ( renderType == Geometry::TYPE_POLYGON && lineSymbol )
            {
                // polygon offset on the poly so the outline doesn't z-fight
                osgGeom->getOrCreateStateSet()->setAttributeAndModes( new osg::PolygonOffset(1,1), 1 );

                osg::ref_ptr<osg::Geometry> outline = new osg::Geometry();
                outline->setUseVertexBufferObjects( _useVertexBufferObjects.value() );

                buildPolygon(part, featureSRS, mapSRS, makeECEF, false, outline);

                if ( outline->getVertexArray()->getVertexBufferObject() )
                    outline->getVertexArray()->getVertexBufferObject()->setUsage(GL_STATIC_DRAW_ARB);    

                // check for explicit tessellation disable:                
                bool disableTess = lineSymbol && lineSymbol->tessellation().isSetTo(0);

                // subdivide if necessary.                
                if ( makeECEF && !disableTess )
                {
                    double threshold = osg::DegreesToRadians( *_maxAngle_deg );
                    OE_DEBUG << "Running mesh subdivider for outlines with threshold " << *_maxAngle_deg << std::endl;
                    MeshSubdivider ms( _world2local, _local2world );
                    if ( input->geoInterp().isSet() )
                        ms.run( *outline, threshold, *input->geoInterp() );
                    else
                        ms.run( *outline, threshold, *_geoInterp );
                }

                // if the line uses a non-pixel width, polygonize it.
                if ( lineSymbol->stroke()->widthUnits() != Units::PIXELS )
                {
                    PolygonizeLinesOperator polygonize( *lineSymbol->stroke() );
                    outline = polygonize( dynamic_cast<osg::Vec3Array*>(outline->getVertexArray()), 0L );
                }
                
                osg::Vec4f outlineColor = lineSymbol->stroke()->color();
                osg::Vec4Array* outlineColors = new osg::Vec4Array();   
                outline->setColorArray(outlineColors);
#if USE_SINGLE_COLOR
                outlineColors->reserve(1);
                outlineColors->push_back( outlineColor );
                outline->setColorBinding( osg::Geometry::BIND_OVERALL );
#else
                unsigned pcount = outline->getVertexArray()->getNumElements();           
                outlineColors->assign( pcount, outlineColor );
                outline->setColorBinding( osg::Geometry::BIND_PER_VERTEX );                
#endif                

                if ( lineSymbol )
                    applyLineSymbology( osgGeom->getOrCreateStateSet(), lineSymbol );

                // make normals before adding an outline
                osgUtil::SmoothingVisitor sv;
                _geode->accept( sv );

                _geode->addDrawable( outline );

                // Mark each primitive set with its feature ID.
                if ( context.featureIndex() )
                    context.featureIndex()->tagPrimitiveSets( outline, input );
            }
#endif
        }
    }
    
    return true;
}

// builds and tessellates a polygon (with or without holes)
void
BuildGeometryFilter::buildPolygon(Geometry*               ring,
                                  const SpatialReference* featureSRS,
                                  const SpatialReference* mapSRS,
                                  bool                    makeECEF,
                                  bool                    tessellate,
                                  osg::Geometry*          osgGeom)
{
    if ( !ring->isValid() )
        return;

    int totalPoints = ring->getTotalPointCount();
    osg::Vec3Array* allPoints = new osg::Vec3Array();
    transformAndLocalize( ring->asVector(), featureSRS, allPoints, mapSRS, _world2local, makeECEF );

    GLenum mode = GL_LINE_LOOP;
    osgGeom->addPrimitiveSet( new osg::DrawArrays( mode, 0, ring->size() ) );

    Polygon* poly = dynamic_cast<Polygon*>(ring);
    if ( poly )
    {
        int offset = ring->size();

        for( RingCollection::const_iterator h = poly->getHoles().begin(); h != poly->getHoles().end(); ++h )
        {
            Geometry* hole = h->get();
            if ( hole->isValid() )
            {
                transformAndLocalize( hole->asVector(), featureSRS, allPoints, mapSRS, _world2local, makeECEF );

                osgGeom->addPrimitiveSet( new osg::DrawArrays( mode, offset, hole->size() ) );
                offset += hole->size();
            }            
        }
    }
    osgGeom->setVertexArray( allPoints );

    if ( tessellate )
    {
        osgUtil::Tessellator tess;
        tess.setTessellationType( osgUtil::Tessellator::TESS_TYPE_GEOMETRY );
        tess.setWindingType( osgUtil::Tessellator::TESS_WINDING_POSITIVE );
        tess.retessellatePolygons( *osgGeom );
    }

    //// Normal computation.
    //// Not completely correct, but better than no normals at all. TODO: update this
    //// to generate a proper normal vector in ECEF mode.
    ////
    //// We cannot accurately rely on triangles from the tessellation, since we could have
    //// very "degraded" triangles (close to a line), and the normal computation would be bad.
    //// In this case, we would have to average the normal vector over each triangle of the polygon.
    //// The Newell's formula is simpler and more direct here.
    //osg::Vec3 normal( 0.0, 0.0, 0.0 );
    //for ( size_t i = 0; i < poly->size(); ++i )
    //{
    //    osg::Vec3 pi = (*poly)[i];
    //    osg::Vec3 pj = (*poly)[ (i+1) % poly->size() ];
    //    normal[0] += ( pi[1] - pj[1] ) * ( pi[2] + pj[2] );
    //    normal[1] += ( pi[2] - pj[2] ) * ( pi[0] + pj[0] );
    //    normal[2] += ( pi[0] - pj[0] ) * ( pi[1] + pj[1] );
    //}
    //normal.normalize();

    //osg::Vec3Array* normals = new osg::Vec3Array();
    //normals->push_back( normal );
    //osgGeom->setNormalArray( normals );
    //osgGeom->setNormalBinding( osg::Geometry::BIND_OVERALL );
}


osg::Node*
BuildGeometryFilter::push( FeatureList& input, FilterContext& context )
{
    reset();

    computeLocalizers( context );

    const LineSymbol*    line = _style.get<LineSymbol>();
    const PolygonSymbol* poly = _style.get<PolygonSymbol>();

    bool ok;
    if ( poly && line )
    {
        Style save(_style);
        _style.remove<LineSymbol>();
        ok = process( input, context );

        _style = save;
        _style.remove<PolygonSymbol>();
        save.remove<PolygonSymbol>();
        ok = process( input, context );

        _style = save;
    }
    else
    {
        ok = process( input, context );
    }

    // convert all geom to triangles and consolidate into minimal set of Geometries
    if ( !_featureNameExpr.isSet() )
    {
        MeshConsolidator::run( *_geode.get() );

        VertexCacheOptimizer vco;
        _geode->accept( vco );
    }

    osg::Node* result = 0L;

    if ( ok )
    {
        if ( !_style.empty() && _geode.valid() )
        {
            // could optimize this to only happen is lines or points were created ..
            const LineSymbol* lineSymbol = _style.getSymbol<LineSymbol>();
            float size = 1.0;
            if (lineSymbol)
                size = std::max(1.0f, lineSymbol->stroke()->width().value());

            _geode->getOrCreateStateSet()->setAttribute( new osg::Point(size), osg::StateAttribute::ON );
            _geode->getOrCreateStateSet()->setAttribute( new osg::LineWidth(size), osg::StateAttribute::ON );

            const PointSymbol* pointSymbol = _style.getSymbol<PointSymbol>();
            if ( pointSymbol && pointSymbol->size().isSet() )
                _geode->getOrCreateStateSet()->setAttribute( 
                    new osg::Point( *pointSymbol->size() ), osg::StateAttribute::ON );
        }

        // apply the delocalization matrix for no-jitter
        result = delocalize( _geode.release() );
    }
    else
    {
        result = 0L;
    }

    return result;
}
