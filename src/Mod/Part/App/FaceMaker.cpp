/***************************************************************************
 *   Copyright (c) 2016 Victor Titov (DeepSOIC) <vv.titov@gmail.com>       *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include "PreCompiled.h"
#ifndef _PreComp_
# include <BRepBuilderAPI_MakeFace.hxx>
# include <BRepBuilderAPI_MakeWire.hxx>
# include <TopoDS.hxx>
# include <TopoDS_Builder.hxx>
# include <TopoDS_Iterator.hxx>
# include <QtGlobal>
#endif

#include <memory>

#include "FaceMaker.h"
#include <App/MappedElement.h>
#include "TopoShape.h"
#include "TopoShapeOpCode.h"


TYPESYSTEM_SOURCE_ABSTRACT(Part::FaceMaker, Base::BaseClass)
TYPESYSTEM_SOURCE_ABSTRACT(Part::FaceMakerPublic, Part::FaceMaker)

void Part::FaceMaker::addWire(const TopoDS_Wire& w)
{
    this->addShape(w);
}

void Part::FaceMaker::addShape(const TopoDS_Shape& sh)
{
    addTopoShape(sh);
}

void Part::FaceMaker::addTopoShape(const TopoShape& shape) {
    const TopoDS_Shape &sh = shape.getShape();
    if(sh.IsNull())
        throw Base::ValueError("Input shape is null.");
    switch(sh.ShapeType()){
        case TopAbs_COMPOUND:
            this->myCompounds.push_back(TopoDS::Compound(sh));
        break;
        case TopAbs_WIRE:
            this->myWires.push_back(TopoDS::Wire(sh));
        break;
        case TopAbs_EDGE:
            this->myWires.push_back(BRepBuilderAPI_MakeWire(TopoDS::Edge(sh)).Wire());
        break;
        case TopAbs_FACE:
            this->myInputFaces.push_back(sh);
        break;
        default:
            throw Base::TypeError("Shape must be a wire, edge or compound. Something else was supplied.");
        break;
    }
    this->mySourceShapes.push_back(shape);
}

void Part::FaceMaker::useCompound(const TopoDS_Compound& comp)
{
    TopoDS_Iterator it(comp);
    for(; it.More(); it.Next()){
        this->addShape(it.Value());
    }
}

void Part::FaceMaker::useTopoCompound(const TopoShape& comp)
{
    for(auto &s : comp.getSubTopoShapes())
        this->addTopoShape(s);
}

const TopoDS_Face& Part::FaceMaker::Face()
{
    return TopoDS::Face(TopoFace().getShape());
}

const Part::TopoShape &Part::FaceMaker::TopoFace() const{
    if(this->myTopoShape.isNull())
        throw NullShapeException("Part::FaceMaker: result shape is null.");
    if (this->myTopoShape.getShape().ShapeType() != TopAbs_FACE)
        throw Base::TypeError("Part::FaceMaker: return shape is not a single face.");
    return this->myTopoShape;
}

const Part::TopoShape &Part::FaceMaker::getTopoShape() const{
    if(this->myTopoShape.isNull())
        throw NullShapeException("Part::FaceMaker: result shape is null.");
    return this->myTopoShape;
}

#if OCC_VERSION_HEX >= 0x070600
void Part::FaceMaker::Build(const Message_ProgressRange&)
#else
void Part::FaceMaker::Build()
#endif
{
    this->NotDone();
    this->myShapesToReturn = this->myInputFaces;
    this->myGenerated.Clear();

    this->Build_Essence();//adds stuff to myShapesToReturn

    for(const TopoDS_Compound& cmp : this->myCompounds){
        std::unique_ptr<FaceMaker> facemaker = Part::FaceMaker::ConstructFromType(this->getTypeId());

        facemaker->useCompound(cmp);

        facemaker->Build();
        const TopoDS_Shape &subfaces = facemaker->Shape();
        if (subfaces.IsNull())
            continue;
        if (subfaces.ShapeType() == TopAbs_COMPOUND){
            this->myShapesToReturn.push_back(subfaces);
        } else {
            //result is not a compound (probably, a face)... but we want to follow compounding structure of input, so wrap it into compound.
            TopoDS_Builder builder;
            TopoDS_Compound cmp_res;
            builder.MakeCompound(cmp_res);
            builder.Add(cmp_res,subfaces);
            this->myShapesToReturn.push_back(cmp_res);
        }
    }

    if(this->myShapesToReturn.empty()){
        //nothing to do, null shape will be returned.
        this->myShape = TopoDS_Shape();
    } else if (this->myShapesToReturn.size() == 1){
        this->myShape = this->myShapesToReturn[0];
    } else {
        TopoDS_Builder builder;
        TopoDS_Compound cmp_res;
        builder.MakeCompound(cmp_res);
        for(TopoDS_Shape &sh: this->myShapesToReturn){
            builder.Add(cmp_res,sh);
        }
        this->myShape = cmp_res;
    }

    postBuild();
}

struct ElementName {
    long tag;
    Data::MappedName name;
    Data::ElementIDRefs sids;

    ElementName(long t, const Data::MappedName &n, const Data::ElementIDRefs &sids)
        :tag(t),name(n), sids(sids)
    {}

    inline bool operator<(const ElementName &other) const {
        if(tag<other.tag)
            return true;
        if(tag>other.tag)
            return false;
        return Data::ElementNameComparator()(name,other.name);
    }
};

void Part::FaceMaker::postBuild() {
    this->myTopoShape.setShape(this->myShape);
    this->myTopoShape.Hasher = this->MyHasher;
    this->myTopoShape.mapSubElement(this->mySourceShapes);
    int index = 0;
    const char *op = this->MyOp;
    if(!op)
        op = Part::OpCodes::Face;
    const auto &faces = this->myTopoShape.getSubTopoShapes(TopAbs_FACE);
    // name the face using the edges of its outer wire
    for(auto &face : faces) {
        ++index;
        TopoShape wire = face.splitWires();
        wire.mapSubElement(face);
        std::set<ElementName> edgeNames;
        int count = wire.countSubShapes(TopAbs_EDGE);
        for (int index2 = 1; index2 <= count; ++index2) {
            Data::ElementIDRefs sids;
            Data::MappedName name =
                face.getMappedName(Data::IndexedName::fromConst("Edge", index2), false, &sids);
            if (!name) {
                continue;
            }
            edgeNames.emplace(wire.getElementHistory(name), name, sids);
        }
        if (edgeNames.empty()) {
            continue;
        }

        std::vector<Data::MappedName> names;
        Data::ElementIDRefs sids;
        // We just use the first source element name to make the face name more
        // stable
        names.push_back(edgeNames.begin()->name);
        sids = edgeNames.begin()->sids;
        this->myTopoShape.setElementComboName(
                Data::IndexedName::fromConst("Face",index),names,op,nullptr,&sids);
    }
    this->myTopoShape.initCache(true);
    this->Done();
}

std::unique_ptr<Part::FaceMaker> Part::FaceMaker::ConstructFromType(const char* className)
{
    Base::Type fmType = Base::Type::fromName(className);
    if (fmType.isBad()){
        std::stringstream ss;
        ss << "Class '"<< className <<"' not found.";
        throw Base::TypeError(ss.str().c_str());
    }
    return Part::FaceMaker::ConstructFromType(fmType);
}

std::unique_ptr<Part::FaceMaker> Part::FaceMaker::ConstructFromType(Base::Type type)
{
    if (!type.isDerivedFrom(Part::FaceMaker::getClassTypeId())){
        std::stringstream ss;
        ss << "Class '" << type.getName() << "' is not derived from Part::FaceMaker.";
        throw Base::TypeError(ss.str().c_str());
    }
    std::unique_ptr<FaceMaker> instance(static_cast<Part::FaceMaker*>(type.createInstance()));
    if (!instance){
        std::stringstream ss;
        ss << "Cannot create FaceMaker from abstract type '" << type.getName() << "'";
        throw Base::TypeError(ss.str().c_str());
    }
    return instance;
}

void Part::FaceMaker::throwNotImplemented()
{
    throw Base::NotImplementedError("Not implemented yet...");
}


//----------------------------------------------------------------------------------------

TYPESYSTEM_SOURCE(Part::FaceMakerSimple, Part::FaceMakerPublic)


std::string Part::FaceMakerSimple::getUserFriendlyName() const
{
    return std::string(QT_TRANSLATE_NOOP("Part_FaceMaker","Simple"));
}

std::string Part::FaceMakerSimple::getBriefExplanation() const
{
    return std::string(QT_TRANSLATE_NOOP("Part_FaceMaker","Makes separate plane face from every wire independently. No support for holes; wires can be on different planes."));
}

void Part::FaceMakerSimple::Build_Essence()
{
    for(TopoDS_Wire &w: myWires){
        this->myShapesToReturn.push_back(BRepBuilderAPI_MakeFace(w).Shape());
    }
}
