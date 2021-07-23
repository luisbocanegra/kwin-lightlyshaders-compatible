/*
    SPDX-FileCopyrightText: 2014 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#include "surface_interface.h"
#include "surface_interface_p.h"
#include "clientbuffer.h"
#include "clientconnection.h"
#include "compositor_interface.h"
#include "display.h"
#include "idleinhibit_v1_interface_p.h"
#include "pointerconstraints_v1_interface_p.h"
#include "region_interface_p.h"
#include "subcompositor_interface.h"
#include "subsurface_interface_p.h"
#include "surfacerole_p.h"
#include "utils.h"
// std
#include <algorithm>

namespace KWaylandServer
{

QList<SurfaceInterface *> SurfaceInterfacePrivate::surfaces;

KWaylandFrameCallback::KWaylandFrameCallback(wl_resource *resource, SurfaceInterface *surface)
    : QtWaylandServer::wl_callback(resource)
    , surface(surface)
{
}

void KWaylandFrameCallback::destroy()
{
    wl_resource_destroy(resource()->handle);
}

void KWaylandFrameCallback::callback_destroy_resource(Resource *)
{
    if (surface) {
        SurfaceInterfacePrivate *surfacePrivate = SurfaceInterfacePrivate::get(surface);
        surfacePrivate->current.frameCallbacks.removeOne(this);
        surfacePrivate->pending.frameCallbacks.removeOne(this);
        surfacePrivate->cached.frameCallbacks.removeOne(this);
    }
    delete this;
}

SurfaceInterfacePrivate::SurfaceInterfacePrivate(SurfaceInterface *q)
    : q(q)
{
    surfaces.append(q);
}

SurfaceInterfacePrivate::~SurfaceInterfacePrivate()
{
    // Need a copy to avoid hitting invalidated iterators in the for loop.
    const QList<KWaylandFrameCallback *> currentFrameCallbacks = current.frameCallbacks;
    for (KWaylandFrameCallback *frameCallback : currentFrameCallbacks) {
        frameCallback->destroy();
    }

    const QList<KWaylandFrameCallback *> pendingFrameCallbacks = pending.frameCallbacks;
    for (KWaylandFrameCallback *frameCallback : pendingFrameCallbacks) {
        frameCallback->destroy();
    }

    const QList<KWaylandFrameCallback *> cachedFrameCallbacks = cached.frameCallbacks;
    for (KWaylandFrameCallback *frameCallback : cachedFrameCallbacks) {
        frameCallback->destroy();
    }

    if (current.buffer) {
        current.buffer->unref();
    }
    surfaces.removeOne(q);
}

void SurfaceInterfacePrivate::addChild(SubSurfaceInterface *child)
{
    // protocol is not precise on how to handle the addition of new sub surfaces
    pending.above.append(child);
    cached.above.append(child);
    current.above.append(child);
    child->surface()->setOutputs(outputs);
    Q_EMIT q->childSubSurfaceAdded(child);
    Q_EMIT q->childSubSurfacesChanged();
}

void SurfaceInterfacePrivate::removeChild(SubSurfaceInterface *child)
{
    // protocol is not precise on how to handle the addition of new sub surfaces
    pending.below.removeAll(child);
    pending.above.removeAll(child);
    cached.below.removeAll(child);
    cached.above.removeAll(child);
    current.below.removeAll(child);
    current.above.removeAll(child);
    Q_EMIT q->childSubSurfaceRemoved(child);
    Q_EMIT q->childSubSurfacesChanged();
}

bool SurfaceInterfacePrivate::raiseChild(SubSurfaceInterface *subsurface, SurfaceInterface *anchor)
{
    Q_ASSERT(subsurface->parentSurface() == q);

    QList<SubSurfaceInterface *> *anchorList;
    int anchorIndex;

    pending.below.removeOne(subsurface);
    pending.above.removeOne(subsurface);

    if (anchor == q) {
        // Pretend as if the parent surface were before the first child in the above list.
        anchorList = &pending.above;
        anchorIndex = -1;
    } else if (anchorIndex = pending.above.indexOf(anchor->subSurface()); anchorIndex != -1) {
        anchorList = &pending.above;
    } else if (anchorIndex = pending.below.indexOf(anchor->subSurface()); anchorIndex != -1) {
        anchorList = &pending.below;
    } else {
        return false; // The anchor belongs to other sub-surface tree.
    }

    anchorList->insert(anchorIndex + 1, subsurface);
    pending.childrenChanged = true;
    return true;
}

bool SurfaceInterfacePrivate::lowerChild(SubSurfaceInterface *subsurface, SurfaceInterface *anchor)
{
    Q_ASSERT(subsurface->parentSurface() == q);

    QList<SubSurfaceInterface *> *anchorList;
    int anchorIndex;

    pending.below.removeOne(subsurface);
    pending.above.removeOne(subsurface);

    if (anchor == q) {
        // Pretend as if the parent surface were after the last child in the below list.
        anchorList = &pending.below;
        anchorIndex = pending.below.count();
    } else if (anchorIndex = pending.above.indexOf(anchor->subSurface()); anchorIndex != -1) {
        anchorList = &pending.above;
    } else if (anchorIndex = pending.below.indexOf(anchor->subSurface()); anchorIndex != -1) {
        anchorList = &pending.below;
    } else {
        return false; // The anchor belongs to other sub-surface tree.
    }

    anchorList->insert(anchorIndex, subsurface);
    pending.childrenChanged = true;
    return true;
}

void SurfaceInterfacePrivate::setShadow(const QPointer<ShadowInterface> &shadow)
{
    pending.shadow = shadow;
    pending.shadowIsSet = true;
}

void SurfaceInterfacePrivate::setBlur(const QPointer<BlurInterface> &blur)
{
    pending.blur = blur;
    pending.blurIsSet = true;
}

void SurfaceInterfacePrivate::setSlide(const QPointer<SlideInterface> &slide)
{
    pending.slide = slide;
    pending.slideIsSet = true;
}

void SurfaceInterfacePrivate::setContrast(const QPointer<ContrastInterface> &contrast)
{
    pending.contrast = contrast;
    pending.contrastIsSet = true;
}

void SurfaceInterfacePrivate::installPointerConstraint(LockedPointerV1Interface *lock)
{
    Q_ASSERT(!lockedPointer);
    Q_ASSERT(!confinedPointer);

    lockedPointer = lock;

    auto cleanUp = [this]() {
        lockedPointer = nullptr;
        QObject::disconnect(constrainsOneShotConnection);
        constrainsOneShotConnection = QMetaObject::Connection();
        QObject::disconnect(constrainsUnboundConnection);
        constrainsUnboundConnection = QMetaObject::Connection();
        Q_EMIT q->pointerConstraintsChanged();
    };

    if (lock->lifeTime() == LockedPointerV1Interface::LifeTime::OneShot) {
        constrainsOneShotConnection = QObject::connect(lock, &LockedPointerV1Interface::lockedChanged, q,
            [this, cleanUp] {
                if (lockedPointer->isLocked()) {
                    return;
                }
                cleanUp();
            }
        );
    }
    constrainsUnboundConnection = QObject::connect(lock, &LockedPointerV1Interface::destroyed, q, cleanUp);
    Q_EMIT q->pointerConstraintsChanged();
}

void SurfaceInterfacePrivate::installPointerConstraint(ConfinedPointerV1Interface *confinement)
{
    Q_ASSERT(!lockedPointer);
    Q_ASSERT(!confinedPointer);

    confinedPointer = confinement;

    auto cleanUp = [this]() {
        confinedPointer = nullptr;
        QObject::disconnect(constrainsOneShotConnection);
        constrainsOneShotConnection = QMetaObject::Connection();
        QObject::disconnect(constrainsUnboundConnection);
        constrainsUnboundConnection = QMetaObject::Connection();
        Q_EMIT q->pointerConstraintsChanged();
    };

    if (confinement->lifeTime() == ConfinedPointerV1Interface::LifeTime::OneShot) {
        constrainsOneShotConnection = QObject::connect(confinement, &ConfinedPointerV1Interface::confinedChanged, q,
            [this, cleanUp] {
                if (confinedPointer->isConfined()) {
                    return;
                }
                cleanUp();
            }
        );
    }
    constrainsUnboundConnection = QObject::connect(confinement, &ConfinedPointerV1Interface::destroyed, q, cleanUp);
    Q_EMIT q->pointerConstraintsChanged();
}

void SurfaceInterfacePrivate::installIdleInhibitor(IdleInhibitorV1Interface *inhibitor)
{
    idleInhibitors << inhibitor;
    QObject::connect(inhibitor, &IdleInhibitorV1Interface::destroyed, q,
        [this, inhibitor] {
            idleInhibitors.removeOne(inhibitor);
            if (idleInhibitors.isEmpty()) {
                Q_EMIT q->inhibitsIdleChanged();
            }
        }
    );
    if (idleInhibitors.count() == 1) {
        Q_EMIT q->inhibitsIdleChanged();
    }
}

void SurfaceInterfacePrivate::surface_destroy_resource(Resource *)
{
    Q_EMIT q->aboutToBeDestroyed();
    delete q;
}

void SurfaceInterfacePrivate::surface_destroy(Resource *resource)
{
    wl_resource_destroy(resource->handle);
}

void SurfaceInterfacePrivate::surface_attach(Resource *resource, struct ::wl_resource *buffer, int32_t x, int32_t y)
{
    Q_UNUSED(resource)
    pending.bufferIsSet = true;
    pending.offset = QPoint(x, y);
    if (!buffer) {
        // got a null buffer, deletes content in next frame
        pending.buffer = nullptr;
        pending.damage = QRegion();
        pending.bufferDamage = QRegion();
        return;
    }
    pending.buffer = compositor->display()->clientBufferForResource(buffer);
}

void SurfaceInterfacePrivate::surface_damage(Resource *, int32_t x, int32_t y, int32_t width, int32_t height)
{
    pending.damage |= QRect(x, y, width, height);
}

void SurfaceInterfacePrivate::surface_frame(Resource *resource, uint32_t callback)
{
    wl_resource *callbackResource = wl_resource_create(resource->client(), &wl_callback_interface,
                                                       /* version */ 1, callback);
    if (!callbackResource) {
        wl_resource_post_no_memory(resource->handle);
        return;
    }
    pending.frameCallbacks.append(new KWaylandFrameCallback(callbackResource, q));
}

void SurfaceInterfacePrivate::surface_set_opaque_region(Resource *resource, struct ::wl_resource *region)
{
    Q_UNUSED(resource)
    RegionInterface *r = RegionInterface::get(region);
    pending.opaque = r ? r->region() : QRegion();
    pending.opaqueIsSet = true;

}

void SurfaceInterfacePrivate::surface_set_input_region(Resource *resource, struct ::wl_resource *region)
{
    Q_UNUSED(resource)
    RegionInterface *r = RegionInterface::get(region);
    pending.input = r ? r->region() : infiniteRegion();
    pending.inputIsSet = true;
}

void SurfaceInterfacePrivate::surface_commit(Resource *resource)
{
    Q_UNUSED(resource)
    if (subSurface) {
        commitSubSurface();
    } else {
        applyState(&pending);
    }
}

void SurfaceInterfacePrivate::surface_set_buffer_transform(Resource *resource, int32_t transform)
{
    if (transform < 0 || transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
        wl_resource_post_error(resource->handle, error_invalid_transform,
                               "buffer transform must be a valid transform (%d specified)", transform);
        return;
    }
    pending.bufferTransform = OutputInterface::Transform(transform);
    pending.bufferTransformIsSet = true;
}

void SurfaceInterfacePrivate::surface_set_buffer_scale(Resource *resource, int32_t scale)
{
    if (scale < 1) {
        wl_resource_post_error(resource->handle, error_invalid_scale,
                               "buffer scale must be at least one (%d specified)", scale);
        return;
    }
    pending.bufferScale = scale;
    pending.bufferScaleIsSet = true;
}

void SurfaceInterfacePrivate::surface_damage_buffer(Resource *resource, int32_t x, int32_t y, int32_t width, int32_t height)
{
    Q_UNUSED(resource)
    pending.bufferDamage |= QRect(x, y, width, height);
}

SurfaceInterface::SurfaceInterface(CompositorInterface *compositor, wl_resource *resource)
    : QObject(compositor)
    , d(new SurfaceInterfacePrivate(this))
{
    d->compositor = compositor;
    d->init(resource);
    d->client = compositor->display()->getConnection(d->resource()->client());
}

SurfaceInterface::~SurfaceInterface()
{
}

uint32_t SurfaceInterface::id() const
{
    return wl_resource_get_id(resource());
}

ClientConnection *SurfaceInterface::client() const
{
    return d->client;
}

wl_resource *SurfaceInterface::resource() const
{
    return d->resource()->handle;
}

CompositorInterface *SurfaceInterface::compositor() const
{
    return d->compositor;
}

QList<SurfaceInterface *> SurfaceInterface::surfaces()
{
    return SurfaceInterfacePrivate::surfaces;
}

void SurfaceInterface::frameRendered(quint32 msec)
{
    // notify all callbacks
    while (!d->current.frameCallbacks.isEmpty()) {
        KWaylandFrameCallback *frameCallback = d->current.frameCallbacks.takeFirst();
        frameCallback->send_done(msec);
        frameCallback->destroy();
    }
    for (SubSurfaceInterface *subsurface : qAsConst(d->current.below)) {
        subsurface->surface()->frameRendered(msec);
    }
    for (SubSurfaceInterface *subsurface : qAsConst(d->current.above)) {
        subsurface->surface()->frameRendered(msec);
    }
}

bool SurfaceInterface::hasFrameCallbacks() const
{
    return !d->current.frameCallbacks.isEmpty();
}

QMatrix4x4 SurfaceInterfacePrivate::buildSurfaceToBufferMatrix()
{
    // The order of transforms is reversed, i.e. the viewport transform is the first one.

    QMatrix4x4 surfaceToBufferMatrix;

    if (!current.buffer) {
        return surfaceToBufferMatrix;
    }

    surfaceToBufferMatrix.scale(current.bufferScale, current.bufferScale);

    switch (current.bufferTransform) {
    case OutputInterface::Transform::Normal:
    case OutputInterface::Transform::Flipped:
        break;
    case OutputInterface::Transform::Rotated90:
    case OutputInterface::Transform::Flipped90:
        surfaceToBufferMatrix.translate(0, bufferSize.height() / current.bufferScale);
        surfaceToBufferMatrix.rotate(-90, 0, 0, 1);
        break;
    case OutputInterface::Transform::Rotated180:
    case OutputInterface::Transform::Flipped180:
        surfaceToBufferMatrix.translate(bufferSize.width() / current.bufferScale,
                                        bufferSize.height() / current.bufferScale);
        surfaceToBufferMatrix.rotate(-180, 0, 0, 1);
        break;
    case OutputInterface::Transform::Rotated270:
    case OutputInterface::Transform::Flipped270:
        surfaceToBufferMatrix.translate(bufferSize.width() / current.bufferScale, 0);
        surfaceToBufferMatrix.rotate(-270, 0, 0, 1);
        break;
    }

    switch (current.bufferTransform) {
    case OutputInterface::Transform::Flipped:
    case OutputInterface::Transform::Flipped180:
        surfaceToBufferMatrix.translate(bufferSize.width() / current.bufferScale, 0);
        surfaceToBufferMatrix.scale(-1, 1);
        break;
    case OutputInterface::Transform::Flipped90:
    case OutputInterface::Transform::Flipped270:
        surfaceToBufferMatrix.translate(bufferSize.height() / current.bufferScale, 0);
        surfaceToBufferMatrix.scale(-1, 1);
        break;
    default:
        break;
    }

    if (current.viewport.sourceGeometry.isValid()) {
        surfaceToBufferMatrix.translate(current.viewport.sourceGeometry.x(), current.viewport.sourceGeometry.y());
        surfaceToBufferMatrix.scale(current.viewport.sourceGeometry.width() / surfaceSize.width(),
                                    current.viewport.sourceGeometry.height() / surfaceSize.height());
    }

    return surfaceToBufferMatrix;
}

void SurfaceState::mergeInto(SurfaceState *target)
{
    if (bufferIsSet) {
        target->buffer = buffer;
        target->offset = offset;
        target->damage = damage;
        target->bufferDamage = bufferDamage;
        target->bufferIsSet = bufferIsSet;
    }
    if (viewport.sourceGeometryIsSet) {
        target->viewport.sourceGeometry = viewport.sourceGeometry;
        target->viewport.sourceGeometryIsSet = true;
    }
    if (viewport.destinationSizeIsSet) {
        target->viewport.destinationSize = viewport.destinationSize;
        target->viewport.destinationSizeIsSet = true;
    }
    if (childrenChanged) {
        target->below = below;
        target->above = above;
        target->childrenChanged = true;
    }
    target->frameCallbacks.append(frameCallbacks);

    if (shadowIsSet) {
        target->shadow = shadow;
        target->shadowIsSet = true;
    }
    if (blurIsSet) {
        target->blur = blur;
        target->blurIsSet = true;
    }
    if (contrastIsSet) {
        target->contrast = contrast;
        target->contrastIsSet = true;
    }
    if (slideIsSet) {
        target->slide = slide;
        target->slideIsSet = true;
    }
    if (inputIsSet) {
        target->input = input;
        target->inputIsSet = true;
    }
    if (opaqueIsSet) {
        target->opaque = opaque;
        target->opaqueIsSet = true;
    }
    if (bufferScaleIsSet) {
        target->bufferScale = bufferScale;
        target->bufferScaleIsSet = true;
    }
    if (bufferTransformIsSet) {
        target->bufferTransform = bufferTransform;
        target->bufferTransformIsSet = true;
    }

    *this = SurfaceState{};
    below = target->below;
    above = target->above;
}

void SurfaceInterfacePrivate::applyState(SurfaceState *next)
{
    const bool bufferChanged = next->bufferIsSet;
    const bool opaqueRegionChanged = next->opaqueIsSet;
    const bool scaleFactorChanged = next->bufferScaleIsSet && (current.bufferScale != next->bufferScale);
    const bool transformChanged = next->bufferTransformIsSet && (current.bufferTransform != next->bufferTransform);
    const bool shadowChanged = next->shadowIsSet;
    const bool blurChanged = next->blurIsSet;
    const bool contrastChanged = next->contrastIsSet;
    const bool slideChanged = next->slideIsSet;
    const bool childrenChanged = next->childrenChanged;
    const bool visibilityChanged = bufferChanged && bool(current.buffer) != bool(next->buffer);

    const QSize oldSurfaceSize = surfaceSize;
    const QSize oldBufferSize = bufferSize;
    const QMatrix4x4 oldSurfaceToBufferMatrix = surfaceToBufferMatrix;
    const QRegion oldInputRegion = inputRegion;

    next->mergeInto(&current);

    if (lockedPointer) {
        auto lockedPointerPrivate = LockedPointerV1InterfacePrivate::get(lockedPointer);
        lockedPointerPrivate->commit();
    }
    if (confinedPointer) {
        auto confinedPointerPrivate = ConfinedPointerV1InterfacePrivate::get(confinedPointer);
        confinedPointerPrivate->commit();
    }

    if (bufferRef != current.buffer) {
        if (bufferRef) {
            bufferRef->unref();
        }
        bufferRef = current.buffer;
        if (bufferRef) {
            bufferRef->ref();
        }
    }

    // TODO: Refactor the state management code because it gets more clumsy.
    if (current.buffer) {
        bufferSize = current.buffer->size();
        if (current.viewport.destinationSize.isValid()) {
            surfaceSize = current.viewport.destinationSize;
        } else if (current.viewport.sourceGeometry.isValid()) {
            surfaceSize = current.viewport.sourceGeometry.size().toSize();
        } else {
            surfaceSize = current.buffer->size() / current.bufferScale;
            switch (current.bufferTransform) {
            case OutputInterface::Transform::Rotated90:
            case OutputInterface::Transform::Rotated270:
            case OutputInterface::Transform::Flipped90:
            case OutputInterface::Transform::Flipped270:
                surfaceSize.transpose();
                break;
            case OutputInterface::Transform::Normal:
            case OutputInterface::Transform::Rotated180:
            case OutputInterface::Transform::Flipped:
            case OutputInterface::Transform::Flipped180:
                break;
            }
        }
    } else {
        surfaceSize = QSize();
        bufferSize = QSize();
    }

    surfaceToBufferMatrix = buildSurfaceToBufferMatrix();
    bufferToSurfaceMatrix = surfaceToBufferMatrix.inverted();
    inputRegion = current.input & QRect(QPoint(0, 0), surfaceSize);
    if (opaqueRegionChanged) {
        Q_EMIT q->opaqueChanged(current.opaque);
    }
    if (oldInputRegion != inputRegion) {
        Q_EMIT q->inputChanged(inputRegion);
    }
    if (scaleFactorChanged) {
        Q_EMIT q->bufferScaleChanged(current.bufferScale);
    }
    if (transformChanged) {
        Q_EMIT q->bufferTransformChanged(current.bufferTransform);
    }
    if (visibilityChanged) {
        if (bufferRef) {
            subSurfaceIsMapped = true;
            Q_EMIT q->mapped();
        } else {
            subSurfaceIsMapped = false;
            Q_EMIT q->unmapped();
        }
    }
    if (bufferChanged) {
        if (current.buffer && (!current.damage.isEmpty() || !current.bufferDamage.isEmpty())) {
            const QRegion windowRegion = QRegion(0, 0, q->size().width(), q->size().height());
            const QRegion bufferDamage = q->mapFromBuffer(current.bufferDamage);
            current.damage = windowRegion.intersected(current.damage.united(bufferDamage));
            Q_EMIT q->damaged(current.damage);
        }
    }
    if (surfaceToBufferMatrix != oldSurfaceToBufferMatrix) {
        Q_EMIT q->surfaceToBufferMatrixChanged();
    }
    if (bufferSize != oldBufferSize) {
        Q_EMIT q->bufferSizeChanged();
    }
    if (surfaceSize != oldSurfaceSize) {
        Q_EMIT q->sizeChanged();
    }
    if (shadowChanged) {
        Q_EMIT q->shadowChanged();
    }
    if (blurChanged) {
        Q_EMIT q->blurChanged();
    }
    if (contrastChanged) {
        Q_EMIT q->contrastChanged();
    }
    if (slideChanged) {
        Q_EMIT q->slideOnShowHideChanged();
    }
    if (childrenChanged) {
        Q_EMIT q->childSubSurfacesChanged();
    }
    // The position of a sub-surface is applied when its parent is committed.
    for (SubSurfaceInterface *subsurface : qAsConst(current.below)) {
        auto subsurfacePrivate = SubSurfaceInterfacePrivate::get(subsurface);
        subsurfacePrivate->parentCommit();
    }
    for (SubSurfaceInterface *subsurface : qAsConst(current.above)) {
        auto subsurfacePrivate = SubSurfaceInterfacePrivate::get(subsurface);
        subsurfacePrivate->parentCommit();
    }
    if (role) {
        role->commit();
    }
    Q_EMIT q->committed();
}

void SurfaceInterfacePrivate::commitSubSurface()
{
    if (subSurface->isSynchronized()) {
        commitToCache();
    } else {
        if (hasCacheState) {
            commitToCache();
            commitFromCache();
        } else {
            applyState(&pending);
        }
    }
}

void SurfaceInterfacePrivate::commitToCache()
{
    pending.mergeInto(&cached);
    hasCacheState = true;
}

void SurfaceInterfacePrivate::commitFromCache()
{
    applyState(&cached);
    hasCacheState = false;
}

QRegion SurfaceInterface::damage() const
{
    return d->current.damage;
}

QRegion SurfaceInterface::opaque() const
{
    return d->current.opaque;
}

QRegion SurfaceInterface::input() const
{
    return d->inputRegion;
}

qint32 SurfaceInterface::bufferScale() const
{
    return d->current.bufferScale;
}

OutputInterface::Transform SurfaceInterface::bufferTransform() const
{
    return d->current.bufferTransform;
}

ClientBuffer *SurfaceInterface::buffer() const
{
    return d->bufferRef;
}

QPoint SurfaceInterface::offset() const
{
    return d->current.offset;
}

SurfaceInterface *SurfaceInterface::get(wl_resource *native)
{
    if (auto surfacePrivate = resource_cast<SurfaceInterfacePrivate *>(native)) {
        return surfacePrivate->q;
    }
    return nullptr;
}

SurfaceInterface *SurfaceInterface::get(quint32 id, const ClientConnection *client)
{
    const QList<SurfaceInterface *> candidates = surfaces();
    for (SurfaceInterface *surface : candidates) {
        if (surface->client() == client && surface->id() == id) {
            return surface;
        }
    }
    return nullptr;
}

QList<SubSurfaceInterface *> SurfaceInterface::below() const
{
    return d->current.below;
}

QList<SubSurfaceInterface *> SurfaceInterface::above() const
{
    return d->current.above;
}

SubSurfaceInterface *SurfaceInterface::subSurface() const
{
    return d->subSurface;
}

QSize SurfaceInterface::size() const
{
    return d->surfaceSize;
}

QRect SurfaceInterface::boundingRect() const
{
    QRect rect(QPoint(0, 0), size());

    for (const SubSurfaceInterface *subSurface : qAsConst(d->current.below)) {
        const SurfaceInterface *childSurface = subSurface->surface();
        rect |= childSurface->boundingRect().translated(subSurface->position());
    }
    for (const SubSurfaceInterface *subSurface : qAsConst(d->current.above)) {
        const SurfaceInterface *childSurface = subSurface->surface();
        rect |= childSurface->boundingRect().translated(subSurface->position());
    }

    return rect;
}

QPointer< ShadowInterface > SurfaceInterface::shadow() const
{
    return d->current.shadow;
}

QPointer< BlurInterface > SurfaceInterface::blur() const
{
    return d->current.blur;
}

QPointer< ContrastInterface > SurfaceInterface::contrast() const
{
    return d->current.contrast;
}

QPointer< SlideInterface > SurfaceInterface::slideOnShowHide() const
{
    return d->current.slide;
}

bool SurfaceInterface::isMapped() const
{
    if (d->subSurface) {
        // from spec:
        // "A sub-surface becomes mapped, when a non-NULL wl_buffer is applied and the parent surface is mapped."
        return d->subSurfaceIsMapped && d->subSurface->parentSurface() && d->subSurface->parentSurface()->isMapped();
    }
    return d->current.buffer != nullptr;
}

QVector<OutputInterface *> SurfaceInterface::outputs() const
{
    return d->outputs;
}

void SurfaceInterface::setOutputs(const QVector<OutputInterface *> &outputs)
{
    QVector<OutputInterface *> removedOutputs = d->outputs;
    for (auto it = outputs.constBegin(), end = outputs.constEnd(); it != end; ++it) {
        const auto o = *it;
        removedOutputs.removeOne(o);
    }
    for (auto it = removedOutputs.constBegin(), end = removedOutputs.constEnd(); it != end; ++it) {
        const auto resources = (*it)->clientResources(client());
        for (wl_resource *outputResource : resources) {
            d->send_leave(outputResource);
        }
        disconnect(d->outputDestroyedConnections.take(*it));
        disconnect(d->outputBoundConnections.take(*it));
    }
    QVector<OutputInterface *> addedOutputsOutputs = outputs;
    for (auto it = d->outputs.constBegin(), end = d->outputs.constEnd(); it != end; ++it) {
        const auto o = *it;
        addedOutputsOutputs.removeOne(o);
    }
    for (auto it = addedOutputsOutputs.constBegin(), end = addedOutputsOutputs.constEnd(); it != end; ++it) {
        const auto o = *it;
        const auto resources = o->clientResources(client());
        for (wl_resource *outputResource : resources) {
            d->send_enter(outputResource);
        }
        d->outputDestroyedConnections[o] = connect(o, &OutputInterface::removed, this, [this, o] {
            auto outputs = d->outputs;
            if (outputs.removeOne(o)) {
                setOutputs(outputs);
            }});

        Q_ASSERT(!d->outputBoundConnections.contains(o));
        d->outputBoundConnections[o] = connect(o, &OutputInterface::bound, this, [this](ClientConnection *c, wl_resource *outputResource) {
            if (c != client()) {
                return;
            }
            d->send_enter(outputResource);
        });
    }

    d->outputs = outputs;
    for (auto child : qAsConst(d->current.below)) {
        child->surface()->setOutputs(outputs);
    }
    for (auto child : qAsConst(d->current.above)) {
        child->surface()->setOutputs(outputs);
    }
}

SurfaceInterface *SurfaceInterface::surfaceAt(const QPointF &position)
{
    if (!isMapped()) {
        return nullptr;
    }

    for (auto it = d->current.above.crbegin(); it != d->current.above.crend(); ++it) {
        const SubSurfaceInterface *current = *it;
        SurfaceInterface *surface = current->surface();
        if (auto s = surface->surfaceAt(position - current->position())) {
            return s;
        }
    }

    // check whether the geometry contains the pos
    if (!size().isEmpty() && QRectF(QPoint(0, 0), size()).contains(position)) {
        return this;
    }

    for (auto it = d->current.below.crbegin(); it != d->current.below.crend(); ++it) {
        const SubSurfaceInterface *current = *it;
        SurfaceInterface *surface = current->surface();
        if (auto s = surface->surfaceAt(position - current->position())) {
            return s;
        }
    }
    return nullptr;
}

SurfaceInterface *SurfaceInterface::inputSurfaceAt(const QPointF &position)
{
    // TODO: Most of this is very similar to SurfaceInterface::surfaceAt
    //       Is there a way to reduce the code duplication?
    if (!isMapped()) {
        return nullptr;
    }

    for (auto it = d->current.above.crbegin(); it != d->current.above.crend(); ++it) {
        const SubSurfaceInterface *current = *it;
        auto surface = current->surface();
        if (auto s = surface->inputSurfaceAt(position - current->position())) {
            return s;
        }
    }

    // check whether the geometry and input region contain the pos
    if (!size().isEmpty() && QRectF(QPoint(0, 0), size()).contains(position) &&
            input().contains(position.toPoint())) {
        return this;
    }

    for (auto it = d->current.below.crbegin(); it != d->current.below.crend(); ++it) {
        const SubSurfaceInterface *current = *it;
        auto surface = current->surface();
        if (auto s = surface->inputSurfaceAt(position - current->position())) {
            return s;
        }
    }

    return nullptr;
}

LockedPointerV1Interface *SurfaceInterface::lockedPointer() const
{
    return d->lockedPointer;
}

ConfinedPointerV1Interface *SurfaceInterface::confinedPointer() const
{
    return d->confinedPointer;
}

bool SurfaceInterface::inhibitsIdle() const
{
    return !d->idleInhibitors.isEmpty();
}

void SurfaceInterface::setDataProxy(SurfaceInterface *surface)
{
    d->dataProxy = surface;
}

SurfaceInterface* SurfaceInterface::dataProxy() const
{
    return d->dataProxy;
}

QPointF SurfaceInterface::mapToBuffer(const QPointF &point) const
{
    return d->surfaceToBufferMatrix.map(point);
}

QPointF SurfaceInterface::mapFromBuffer(const QPointF &point) const
{
    return d->bufferToSurfaceMatrix.map(point);
}

static QRegion map_helper(const QMatrix4x4 &matrix, const QRegion &region)
{
    QRegion result;
    for (const QRect &rect : region) {
        result += matrix.mapRect(rect);
    }
    return result;
}

QRegion SurfaceInterface::mapToBuffer(const QRegion &region) const
{
    return map_helper(d->surfaceToBufferMatrix, region);
}

QRegion SurfaceInterface::mapFromBuffer(const QRegion &region) const
{
    return map_helper(d->bufferToSurfaceMatrix, region);
}

QMatrix4x4 SurfaceInterface::surfaceToBufferMatrix() const
{
    return d->surfaceToBufferMatrix;
}

QPointF SurfaceInterface::mapToChild(SurfaceInterface *child, const QPointF &point) const
{
    QPointF local = point;
    SurfaceInterface *surface = child;

    while (true) {
        if (surface == this) {
            return local;
        }

        SubSurfaceInterface *subsurface = surface->subSurface();
        if (Q_UNLIKELY(!subsurface)) {
            return QPointF();
        }

        local -= subsurface->position();
        surface = subsurface->parentSurface();
    }

    return QPointF();
}


QSize SurfaceInterface::bufferSize() const
{
    return d->bufferSize;
}

} // namespace KWaylandServer
