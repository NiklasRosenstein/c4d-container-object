/// Copyright (C) 2013-2015, Niklas Rosenstein
/// All rights reserved.
///
/// Licensed under the GNU Lesser General Public License.
///
/// \file ContainerObject.cpp

#include "ContainerObject.h"

/// Cinema 4D Includes
#include <c4d.h>
#include <c4d_apibridge.h>
#include <lib_clipmap.h>
#include <lib_iconcollection.h>

/// Resource Symbols
#include <Ocontainer.h>
#include "res/c4d_symbols.h"

#include "Utils/Misc.h"
#include "Utils/AABB.h"


using c4d_apibridge::GetDescriptionID;
using c4d_apibridge::IsEmpty;

/// ***************************************************************************
/// ***************************************************************************
template <typename T>
static inline T* GetNodeData(GeListNode* node) {
  #if API_VERSION >= 17000
    return node->GetNodeData<T>();
  #else
    return static_cast<T*>(node->GetNodeData());
  #endif
}


/// ***************************************************************************
/// This function recursive hides or unhides a node and all its following
/// nodes in the same hierarchy level and below object manager and timeline.
/// Only direct children are hidden or revealed, no other branches.
/// @param[in] root The node to start with.
/// @param[in] hide \c true if the hierarchy should be hidden, \c false
///     if it should be revealed by this function.
/// @param[in] doc The BaseDocument to add undos to, if desired. Pass
///     \c nullptr if no undos should be created.
/// @param[in] sameLevel If \c true (default), all objects following *root*
///     in the hierarchy will also be processed by this function.
/// ***************************************************************************
static void HideHierarchy(BaseList2D* root, Bool hide, BaseDocument* doc, Bool sameLevel=true)
{
  while (root)
  {
    if (doc)
      doc->AddUndo(UNDOTYPE_BITS, root);
    const NBITCONTROL control = (hide ? NBITCONTROL_SET : NBITCONTROL_CLEAR);
    root->ChangeNBit(NBIT_OHIDE, control);
    root->ChangeNBit(NBIT_TL1_HIDE, control);
    root->ChangeNBit(NBIT_TL2_HIDE, control);
    root->ChangeNBit(NBIT_TL3_HIDE, control);
    root->ChangeNBit(NBIT_TL4_HIDE, control);
    root->ChangeNBit(NBIT_THIDE, control);
    root->DelBit(BIT_ACTIVE);

    Bool hideChildren = true;
    if (root->IsInstanceOf(Obase))
    {
      BaseObject* op = static_cast<BaseObject*>(root);
      BaseContainer* bc = op->GetDataInstance();
      CriticalAssert(bc);

      if (!IsEmpty(bc->GetString(CONTAINEROBJECT_PROTECTIONHASH)))
        // Don't modify the hierarchy of "protected" Null-Objects.
        hideChildren = false;
      else if (ContainerIsProtected(op))
        // Don't modify the hierarchy of protected Containers.
        hideChildren = false;
    }

    if (hideChildren)
      HideHierarchy(static_cast<BaseList2D*>(root->GetDown()), hide, doc);

    if (!sameLevel) break;
    root = root->GetNext();
  }
}


/// ***************************************************************************
/// This function hides or unhides all materials used by the object *op*.
/// If *doc* is not \c nullptr, undos will be added.
/// ***************************************************************************
static void HideMaterials(BaseObject* op, Bool hide, BaseDocument* doc)
{
  BaseTag* tag = op->GetFirstTag();
  GeData data;
  while (tag)
  {
    if (tag->GetType() == Ttexture && tag->GetParameter(TEXTURETAG_MATERIAL, data, DESCFLAGS_GET_0))
    {
      BaseMaterial* mat = static_cast<BaseMaterial*>(data.GetLink(doc, Mbase));
      if (mat) HideHierarchy(mat, hide, doc, false);
    }
    tag = tag->GetNext();
  }
  BaseObject* child = op->GetDown();
  while (child) {
    HideMaterials(child, hide, doc);
    child = child->GetNext();
  }
}


/// ***************************************************************************
/// ***************************************************************************
class ContainerObject : public ObjectData
{
  typedef ObjectData super;

  BaseBitmap* m_customIcon;
  Bool m_protected;
  String m_protectionHash;
  friend Bool ContainerIsProtected(BaseObject*, String*);
  friend Bool ContainerProtect(BaseObject*, String const&, String, Bool);
public:

  static NodeData* Alloc() { return gNew(ContainerObject); }

  /// Called from Message() for MSG_DESCRIPTION_COMMAND.
  void OnDescriptionCommand(BaseObject* op, DescriptionCommand* cmdData)
  {
    BaseDocument* doc = op->GetDocument();
    const AutoUndo au(doc);
    const LONG id = GetDescriptionID(cmdData)[0].id;

    switch (id)
    {
      case NRCONTAINER_PACKUP:
        ToggleProtect(op);
        break;
      case NRCONTAINER_ICON_LOAD:
      {
        if (m_protected) break;

        // Ask the user for an image-file.
        Filename flname;
        flname.SetDirectory(GeGetC4DPath(C4D_PATH_DESKTOP));
        Bool ok = flname.FileSelect(FILESELECTTYPE_IMAGES, FILESELECT_LOAD,
            GeLoadString(IDS_SELECTICON));

        if (ok)
        {
          // Ensure the destination bitmap is allocated.
          if (!m_customIcon)
            m_customIcon = BaseBitmap::Alloc();
          else
            m_customIcon->FlushAll();

          // If it is still null here, allocation failed.
          if (!m_customIcon)
            MessageDialog(GeLoadString(IDS_INFO_OUTOFMEMORY));
          else
          {
            IMAGERESULT res = m_customIcon->Init(flname);
            if (res != IMAGERESULT_OK)
            {
              MessageDialog(IDS_INFO_INVALIDIMAGE);
              BaseBitmap::Free(m_customIcon);
            }
            else
            {
              // Scale the bitmap down to 64x64 pixels.
              BaseBitmap* dest = BaseBitmap::Alloc();
              const LONG size = CONTAINEROBJECT_ICONSIZE;
              dest->Init(size, size);
              m_customIcon->ScaleIt(dest, 256, true, true);
              BaseBitmap::Free(m_customIcon);
              m_customIcon = dest;
            }
          }
        }
        break;
      }
      case NRCONTAINER_ICON_CLEAR:
      {
        if (m_protected) break;
        if (m_customIcon)
        {
          // TODO: We possibly require a flag for removing the icon
          // on the next MSG_GETCUSTOMICON message, because Cinema
          // still references this bitmap.
          BaseBitmap::Free(m_customIcon);
        }
        break;
      }
    }
  }

  /// Called from Message() for MSG_GETCUSTOMICON.
  void OnGetCustomIcon(BaseObject* op, GetCustomIconData* data)
  {
    IconData* dIcon = data->dat;
    BaseBitmap* bmp;
    LONG xoff, yoff, xdim, ydim;

    if (m_customIcon)
    {
      if (dIcon->bmp)
      {
        // We can not free the previous bitmap, because it leads to a
        // crash. We copy the custom icon bitmap to the already
        // present bitmap.
        bmp = dIcon->bmp;
        m_customIcon->CopyTo(bmp);
      }
      else
      {
        bmp = m_customIcon->GetClone();
      }
      xoff = 0;
      yoff = 0;
      xdim = bmp->GetBw();
      ydim = bmp->GetBh();
    }
    else
    {
      bmp = dIcon->bmp;
      if (!bmp)
      {
        bmp = BaseBitmap::Alloc();
        bmp->Init(64, 64);
      }
      if (GetIcon(Ocontainer, dIcon))
      {
        dIcon->bmp->CopyTo(bmp);
      }
      xoff = dIcon->x;
      yoff = dIcon->y;
      xdim = dIcon->w;
      ydim = dIcon->h;
    }

    if (bmp)
    {
      // Adjust the IconData.
      dIcon->x = xoff;
      dIcon->y = yoff;
      dIcon->w = xdim;
      dIcon->h = ydim;
      dIcon->bmp = bmp;
      data->filled = true;
    }
    else
    {
      data->filled = false;
    }
  }

  /// Called from Message() for MSG_EDIT (when a user double-clicks
  /// the object icon). Toggles the protection state of the container.
  void ToggleProtect(BaseObject* op)
  {
    BaseDocument* doc = op->GetDocument();
    if (doc)
    {
      doc->StartUndo();
      doc->AddUndo(UNDOTYPE_CHANGE_SMALL, op);
      doc->EndUndo();
    }

    BaseContainer const* bc = op->GetDataInstance();
    if (!bc) return;

    if (!m_protected)
    {
      String password;
      if (!PasswordDialog(&password, false, true)) return;
      String hashed = HashString(password);
      m_protected = true;
      m_protectionHash = hashed;

      HideNodes(op, doc, true);
    }
    else
    {
      String password;
      Bool unlock = false;
      String emptyPassHash = HashString("");
      if (m_protectionHash == emptyPassHash)
      {
        unlock = true;
      }
      else if (PasswordDialog(&password, true, true))
      {
        unlock = (m_protectionHash == HashString(password));
        if (!unlock)
          MessageDialog(GeLoadString(IDS_PASSWORD_INVALID));
      }
      if (unlock)
      {
        m_protected = false;
        HideNodes(op, doc, false);
      }
    }

    op->Message(MSG_CHANGE);
    op->SetDirty(DIRTYFLAGS_DESCRIPTION);
    EventAdd();
  }

  /// Called to hide/unhide the container object contents.
  void HideNodes(BaseObject* op, BaseDocument* doc, Bool hide)
  {
    if (hide)
    {
      BaseContainer* bc = op->GetDataInstance();
      CriticalAssert(bc != nullptr);
      HideHierarchy(op->GetDown(), true, doc);
      if (bc->GetBool(NRCONTAINER_HIDE_TAGS))
        HideHierarchy(op->GetFirstTag(), true, doc);
      if (bc->GetBool(NRCONTAINER_HIDE_MATERIALS))
        HideMaterials(op, true, doc);
    }
    else
    {
      HideHierarchy(op->GetDown(), false, doc);
      HideHierarchy(op->GetFirstTag(), false, doc);
      HideMaterials(op, false, doc);
    }
  }

  // ObjectData Overrides

  virtual void GetDimension(BaseObject* op, Vector* mp, Vector* rad) override
  {
    // Find the Minimum/Maximum of the object's bounding
    // box by all hidden child-objects in its hierarchy.
    AABB bbox;
    for (NodeIterator<BaseObject> it(op->GetDown(), op); it; ++it)
    {
      // We skip objects that are being controlled by
      // a generator object.
      if (it->GetInfo() & OBJECT_GENERATOR && !IsControlledByGenerator(*it))
        bbox.Expand(*it, it->GetMg(), false);
    }

    *mp = bbox.GetMidpoint();
    *rad = bbox.GetSize();
  }

  //  NodeData Overrides

  virtual Bool Init(GeListNode* node) override
  {
    if (!node || !super::Init(node)) return false;
    if (m_customIcon) BaseBitmap::Free(m_customIcon);
    m_protected = false;
    m_protectionHash = "";
    BaseContainer* bc = ((BaseList2D*) node)->GetDataInstance();
    if (!bc) return false;
    bc->SetBool(NRCONTAINER_HIDE_TAGS, false);
    bc->SetBool(NRCONTAINER_HIDE_MATERIALS, true);
    bc->SetBool(NRCONTAINER_GENERATOR_CHECKMARK, true);
    bc->SetString(NRCONTAINER_INFO_NAME, ""_s);
    bc->SetString(NRCONTAINER_INFO_VERSION, ""_s);
    bc->SetString(NRCONTAINER_INFO_URL, ""_s);
    bc->SetString(NRCONTAINER_INFO_AUTHOR, ""_s);
    bc->SetString(NRCONTAINER_INFO_AUTHOR_EMAIL, ""_s);
    bc->SetString(NRCONTAINER_INFO_DESCRIPTION, ""_s);
    return true;
  }

  virtual void Free(GeListNode* node) override
  {
    super::Free(node);
    if (m_customIcon) BaseBitmap::Free(m_customIcon);
  }

  virtual Bool Read(GeListNode* node, HyperFile* hf, LONG level) override
  {
    Bool result = super::Read(node, hf, level);
    if (!result) return result;

    // VERSION 0

    // Read the custom icon from the HyperFile.
    Bool hasImage;
    if (!hf->ReadBool(&hasImage)) return false;

    if (hasImage)
    {
      if (m_customIcon)
        m_customIcon->FlushAll();
      else
        m_customIcon = BaseBitmap::Alloc();
      if (!hf->ReadImage(m_customIcon)) return false;
    }
    else if (m_customIcon)
      BaseBitmap::Free(m_customIcon);

    // VERSION 1000

    if (level >= 1000)
    {
      if (!hf->ReadBool(&m_protected)) return false;
      if (m_protected)
      {
        if (!hf->ReadString(&m_protectionHash)) return false;
      }
    }

    return result;
  }

  virtual Bool Write(GeListNode* node, HyperFile* hf) override
  {
    Bool result = super::Write(node, hf);
    if (!result) return result;

    // VERSION 0

    // Write the custom icon to the HyperFile.
    if (!hf->WriteBool(m_customIcon != NULL)) return false;
    if (m_customIcon)
    {
      if (!hf->WriteImage(m_customIcon, FILTER_PNG, NULL, SAVEBIT_ALPHA))
        return false;
    }

    // VERSION 1000

    if (!hf->WriteBool(m_protected)) return false;
    if (m_protected)
    {
      if (!hf->WriteString(m_protectionHash)) return false;
    }

    return result;
  }

  virtual Bool Message(GeListNode* node, LONG msgType, void* pData) override
  {
    Bool result = super::Message(node, msgType, pData);
    if (!result) return result;

    BaseObject* op = (BaseObject*) node;
    switch (msgType)
    {
      case MSG_DESCRIPTION_COMMAND:
        OnDescriptionCommand(op, (DescriptionCommand*) pData);
        break;
      case MSG_GETCUSTOMICON:
        OnGetCustomIcon(op, (GetCustomIconData*) pData);
        break;
      case MSG_EDIT:
        ToggleProtect(op);
        break;
      default:
        break;
    }
    return result;
  }

  virtual Bool CopyTo(NodeData* nDest, GeListNode* node, GeListNode* destNode,
        COPYFLAGS flags, AliasTrans* at) override
  {
    Bool result = super::CopyTo(nDest, node, destNode, flags, at);
    if (!result) return result;
    ContainerObject* dest = (ContainerObject*) nDest;

    // Copy the custom icon to the new NodeData.
    if (dest->m_customIcon)
      BaseBitmap::Free(dest->m_customIcon);
    if (m_customIcon)
      dest->m_customIcon = m_customIcon->GetClone();

    // And the other stuff.. :-)
    dest->m_protected = m_protected;
    dest->m_protectionHash = m_protectionHash;

    return result;
  }

  virtual Bool GetDDescription(GeListNode* node, Description* desc,
        DESCFLAGS_DESC& flags) override
  {
    if (!node || !desc) return false;
    if (!desc->LoadDescription(Ocontainer)) return false;

    // Hide the Objects parameter group.
    AutoAlloc<AtomArray> t_arr;
    BaseContainer* bc_group = desc->GetParameterI(ID_OBJECTPROPERTIES, t_arr);

    if (bc_group) bc_group->SetBool(DESC_HIDE, m_protected);

    flags |= DESCFLAGS_DESC_LOADED;
    return true;
  }

  virtual Bool GetDParameter(GeListNode* node, const DescID& id, GeData& data,
        DESCFLAGS_GET& flags) override
  {
    switch (id[0].id) {
      case NRCONTAINER_DEV_INFO:
        data.SetString(""_s);
        flags |= DESCFLAGS_GET_PARAM_GET;
        return true;
    }
    return super::GetDParameter(node, id, data, flags);
  }

  virtual Bool SetDParameter(GeListNode* node, const DescID& id,
        const GeData& data, DESCFLAGS_SET& flags) override
  {
    switch (id[0].id) {
      case NRCONTAINER_INFO_NAME:
      case NRCONTAINER_INFO_VERSION:
      case NRCONTAINER_INFO_URL:
      case NRCONTAINER_INFO_AUTHOR:
      case NRCONTAINER_INFO_AUTHOR_EMAIL:
      case NRCONTAINER_INFO_DESCRIPTION:
        if (this->m_protected) {
          // Don't allow to override the existing values.
          flags |= DESCFLAGS_SET_PARAM_SET;
          return true;
        }
        break;
    }
    return super::SetDParameter(node, id, data, flags);
  }

  virtual Bool GetDEnabling(GeListNode* node, const DescID& id,
        const GeData& t_data, DESCFLAGS_ENABLE flags,
        const BaseContainer* itemdesc) override
  {
    switch (id[0].id) {
      case NRCONTAINER_INFO_NAME:
      case NRCONTAINER_INFO_VERSION:
      case NRCONTAINER_INFO_URL:
      case NRCONTAINER_INFO_AUTHOR:
      case NRCONTAINER_INFO_AUTHOR_EMAIL:
      case NRCONTAINER_INFO_DESCRIPTION:
        return !this->m_protected;
    }
    return super::GetDEnabling(node, id, t_data, flags, itemdesc);
  }

  virtual void GetBubbleHelp(GeListNode* node, c4d_apibridge::String& str) override
  {
    super::GetBubbleHelp(node, str);
  }

};


/// ***************************************************************************
/// ***************************************************************************
Bool ContainerIsProtected(BaseObject* op, String* hash)
{
  if (!op || op->GetType() != Ocontainer) return false;
  ContainerObject* data = GetNodeData<ContainerObject>(op);
  if (!data) return false;
  if (data->m_protected)
  {
    if (hash)
      *hash = data->m_protectionHash;
    return true;
  }
  return false;
}

/// ***************************************************************************
/// ***************************************************************************
Bool ContainerProtect(BaseObject* op, String const& pass, String hash, Bool packup)
{
  if (!op || op->GetType() != Ocontainer) return false;
  ContainerObject* data = GetNodeData<ContainerObject>(op);
  if (!data) return false;
  if (data->m_protected)
    return false;
  if (IsEmpty(hash))
    hash = HashString(pass);
  data->m_protected = true;
  data->m_protectionHash = hash;
  if (packup)
    data->HideNodes(op, nullptr, packup);
  return true;
}

/// ***************************************************************************
/// Hook to modify the container object info bitmask based on the parameters.
/// ***************************************************************************
decltype(C4D_Object::GetInfo) _orig_GetInfo = nullptr;
static LONG _hook_GetInfo(GeListNode* op)
{
  if (op && op->GetType() == Ocontainer) {
    GeData data;
    op->GetParameter(NRCONTAINER_GENERATOR_CHECKMARK, data, DESCFLAGS_GET_0);
    if (data.GetBool())
      return OBJECT_GENERATOR;
    else
      return 0;
  }
  return _orig_GetInfo(op);
}


/// ***************************************************************************
/// ***************************************************************************
Bool RegisterContainerObject(Bool menu)
{
  if (menu) {
    BaseContainer* menu = nullptr;
    FindMenuResource("M_EDITOR", "IDS_MENU_OBJECT", &menu);
    if (menu) {
      menu->InsData(MENURESOURCE_SEPERATOR, true);
      menu->InsData(MENURESOURCE_COMMAND, "PLUGIN_CMD_" + LongToString(Ocontainer));
    }
    return true;
  }

  _orig_GetInfo = C4DOS.Bo->GetInfo;
  C4DOS.Bo->GetInfo = _hook_GetInfo;

  AutoAlloc<BaseBitmap> bmp;
  bmp->Init(GeGetPluginPath() + "res" + "img" + "ocontainer.png");

  return RegisterObjectPlugin(
    Ocontainer,
    GeLoadString(IDS_OCONTAINER),
    OBJECT_GENERATOR,
    ContainerObject::Alloc,
    "Ocontainer"_s,
    bmp,
    CONTAINEROBJECT_DISKLEVEL);
}
