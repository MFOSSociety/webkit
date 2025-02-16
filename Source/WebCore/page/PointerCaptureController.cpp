/*
 * Copyright (C) 2019 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "PointerCaptureController.h"

#if ENABLE(POINTER_EVENTS)

#include "Document.h"
#include "Element.h"
#include "EventHandler.h"
#include "EventNames.h"
#include "EventTarget.h"
#include "Page.h"
#include "PointerEvent.h"
#include <wtf/CheckedArithmetic.h>

#if ENABLE(POINTER_LOCK)
#include "PointerLockController.h"
#endif

namespace WebCore {

PointerCaptureController::PointerCaptureController(Page& page)
    : m_page(page)
{
    reset();
}

ExceptionOr<void> PointerCaptureController::setPointerCapture(Element* capturingTarget, PointerID pointerId)
{
    // https://w3c.github.io/pointerevents/#setting-pointer-capture

    // 1. If the pointerId provided as the method's argument does not match any of the active pointers, then throw a DOMException with the name NotFoundError.
    auto iterator = m_activePointerIdsToCapturingData.find(pointerId);
    if (iterator == m_activePointerIdsToCapturingData.end())
        return Exception { NotFoundError };

    // 2. If the Element on which this method is invoked is not connected, throw an exception with the name InvalidStateError.
    if (!capturingTarget->isConnected())
        return Exception { InvalidStateError };

#if ENABLE(POINTER_LOCK)
    // 3. If this method is invoked while the document has a locked element, throw an exception with the name InvalidStateError.
    if (auto* page = capturingTarget->document().page()) {
        if (page->pointerLockController().isLocked())
            return Exception { InvalidStateError };
    }
#endif

    // 4. If the pointer is not in the active buttons state, then terminate these steps.
    // 5. For the specified pointerId, set the pending pointer capture target override to the Element on which this method was invoked.
    auto& capturingData = iterator->value;
    if (capturingData.pointerIsPressed)
        capturingData.pendingTargetOverride = capturingTarget;

    return { };
}

ExceptionOr<void> PointerCaptureController::releasePointerCapture(Element* capturingTarget, PointerID pointerId)
{
    // https://w3c.github.io/pointerevents/#releasing-pointer-capture

    // Pointer capture is released on an element explicitly by calling the element.releasePointerCapture(pointerId) method.
    // When this method is called, a user agent MUST run the following steps:

    // 1. If the pointerId provided as the method's argument does not match any of the active pointers and these steps are not
    // being invoked as a result of the implicit release of pointer capture, then throw a DOMException with the name NotFoundError.
    auto iterator = m_activePointerIdsToCapturingData.find(pointerId);
    if (iterator == m_activePointerIdsToCapturingData.end())
        return Exception { NotFoundError };

    // 2. If hasPointerCapture is false for the Element with the specified pointerId, then terminate these steps.
    if (!hasPointerCapture(capturingTarget, pointerId))
        return { };

    // 3. For the specified pointerId, clear the pending pointer capture target override, if set.
    iterator->value.pendingTargetOverride = nullptr;

    return { };
}

bool PointerCaptureController::hasPointerCapture(Element* capturingTarget, PointerID pointerId)
{
    // https://w3c.github.io/pointerevents/#dom-element-haspointercapture

    // Indicates whether the element on which this method is invoked has pointer capture for the pointer identified by the argument pointerId.
    // In particular, returns true if the pending pointer capture target override for pointerId is set to the element on which this method is
    // invoked, and false otherwise.

    auto iterator = m_activePointerIdsToCapturingData.find(pointerId);
    return iterator != m_activePointerIdsToCapturingData.end() && iterator->value.pendingTargetOverride == capturingTarget;
}

void PointerCaptureController::pointerLockWasApplied()
{
    // https://w3c.github.io/pointerevents/#implicit-release-of-pointer-capture

    // When a pointer lock is successfully applied on an element, a user agent MUST run the steps as if the releasePointerCapture()
    // method has been called if any element is set to be captured or pending to be captured.
    for (auto& capturingData : m_activePointerIdsToCapturingData.values()) {
        capturingData.pendingTargetOverride = nullptr;
        capturingData.targetOverride = nullptr;
    }
}

void PointerCaptureController::elementWasRemoved(Element& element)
{
    for (auto& keyAndValue : m_activePointerIdsToCapturingData) {
        auto& capturingData = keyAndValue.value;
        if (capturingData.pendingTargetOverride == &element || capturingData.targetOverride == &element) {
            // https://w3c.github.io/pointerevents/#implicit-release-of-pointer-capture
            // When the pointer capture target override is no longer connected, the pending pointer capture target override and pointer capture target
            // override nodes SHOULD be cleared and also a PointerEvent named lostpointercapture corresponding to the captured pointer SHOULD be fired
            // at the document.
            ASSERT(WTF::isInBounds<PointerID>(keyAndValue.key));
            auto pointerId = static_cast<PointerID>(keyAndValue.key);
            auto pointerType = capturingData.pointerType;
            releasePointerCapture(&element, pointerId);
            element.document().enqueueDocumentEvent(PointerEvent::create(eventNames().lostpointercaptureEvent, pointerId, pointerType));
            return;
        }
    }
}

void PointerCaptureController::reset()
{
    m_activePointerIdsToCapturingData.clear();
#if !ENABLE(TOUCH_EVENTS)
    CapturingData capturingData;
    capturingData.pointerType = PointerEvent::mousePointerType();
    m_activePointerIdsToCapturingData.add(mousePointerID, capturingData);
#endif
}

void PointerCaptureController::touchWithIdentifierWasRemoved(PointerID pointerId)
{
    m_activePointerIdsToCapturingData.remove(pointerId);
}

bool PointerCaptureController::hasCancelledPointerEventForIdentifier(PointerID pointerId)
{
    auto iterator = m_activePointerIdsToCapturingData.find(pointerId);
    return iterator != m_activePointerIdsToCapturingData.end() && iterator->value.cancelled;
}

bool PointerCaptureController::preventsCompatibilityMouseEventsForIdentifier(PointerID pointerId)
{
    auto iterator = m_activePointerIdsToCapturingData.find(pointerId);
    return iterator != m_activePointerIdsToCapturingData.end() && iterator->value.preventsCompatibilityMouseEvents;
}

#if ENABLE(TOUCH_EVENTS) && PLATFORM(IOS_FAMILY)
void PointerCaptureController::dispatchEventForTouchAtIndex(EventTarget& target, const PlatformTouchEvent& platformTouchEvent, unsigned index, bool isPrimary, WindowProxy& view)
{
    auto dispatchOverOrOutEvent = [&](const String& type) {
        dispatchEvent(PointerEvent::create(type, platformTouchEvent, index, isPrimary, view), &target);
    };

    auto dispatchEnterOrLeaveEvent = [&](const String& type) {
        if (!is<Element>(&target))
            return;

        auto* targetElement = &downcast<Element>(target);

        bool hasCapturingListenerInHierarchy = false;
        for (ContainerNode* curr = targetElement; curr; curr = curr->parentInComposedTree()) {
            if (curr->hasCapturingEventListeners(type)) {
                hasCapturingListenerInHierarchy = true;
                break;
            }
        }

        Vector<Ref<Element>, 32> targetChain;
        for (Element* element = targetElement; element; element = element->parentElementInComposedTree()) {
            if (hasCapturingListenerInHierarchy || element->hasEventListeners(type))
                targetChain.append(*element);
        }

        if (type == eventNames().pointerenterEvent) {
            for (auto& element : WTF::makeReversedRange(targetChain))
                dispatchEvent(PointerEvent::create(type, platformTouchEvent, index, isPrimary, view), element.ptr());
        } else {
            for (auto& element : targetChain)
                dispatchEvent(PointerEvent::create(type, platformTouchEvent, index, isPrimary, view), element.ptr());
        }
    };

    auto pointerEvent = PointerEvent::create(platformTouchEvent, index, isPrimary, view);

    if (pointerEvent->type() == eventNames().pointerdownEvent) {
        // https://w3c.github.io/pointerevents/#the-pointerdown-event
        // For input devices that do not support hover, a user agent MUST also fire a pointer event named pointerover followed by a pointer event named
        // pointerenter prior to dispatching the pointerdown event.
        dispatchOverOrOutEvent(eventNames().pointeroverEvent);
        dispatchEnterOrLeaveEvent(eventNames().pointerenterEvent);
    }

    dispatchEvent(pointerEvent, &target);

    if (pointerEvent->type() == eventNames().pointerupEvent) {
        // https://w3c.github.io/pointerevents/#the-pointerup-event
        // For input devices that do not support hover, a user agent MUST also fire a pointer event named pointerout followed by a
        // pointer event named pointerleave after dispatching the pointerup event.
        dispatchOverOrOutEvent(eventNames().pointeroutEvent);
        dispatchEnterOrLeaveEvent(eventNames().pointerleaveEvent);
    }
}
#endif

RefPtr<PointerEvent> PointerCaptureController::pointerEventForMouseEvent(const MouseEvent& mouseEvent)
{
    const auto& type = mouseEvent.type();
    const auto& names = eventNames();

    auto iterator = m_activePointerIdsToCapturingData.find(mousePointerID);
    ASSERT(iterator != m_activePointerIdsToCapturingData.end());
    auto& capturingData = iterator->value;

    short newButton = mouseEvent.button();
    short button = (type == names.mousemoveEvent && newButton == capturingData.previousMouseButton) ? -1 : newButton;

    // https://w3c.github.io/pointerevents/#chorded-button-interactions
    // Some pointer devices, such as mouse or pen, support multiple buttons. In the Mouse Event model, each button
    // press produces a mousedown and mouseup event. To better abstract this hardware difference and simplify
    // cross-device input authoring, Pointer Events do not fire overlapping pointerdown and pointerup events
    // for chorded button presses (depressing an additional button while another button on the pointer device is
    // already depressed).
    if (type == names.mousedownEvent || type == names.mouseupEvent) {
        // We're already active and getting another mousedown, this means that we should dispatch
        // a pointermove event and let the button state show the newly depressed button.
        if (type == names.mousedownEvent && capturingData.pointerIsPressed)
            return PointerEvent::create(names.pointermoveEvent, button, mouseEvent);

        // We're active and the mouseup still has some pressed button, this means we should dispatch
        // a pointermove event.
        if (type == names.mouseupEvent && capturingData.pointerIsPressed && mouseEvent.buttons() > 0)
            return PointerEvent::create(names.pointermoveEvent, button, mouseEvent);
    }

    capturingData.previousMouseButton = newButton;

    return PointerEvent::create(button, mouseEvent);
}

void PointerCaptureController::dispatchEvent(PointerEvent& event, EventTarget* target)
{
    if (!target || event.target())
        return;

    // https://w3c.github.io/pointerevents/#firing-events-using-the-pointerevent-interface
    // If the event is not gotpointercapture or lostpointercapture, run Process Pending Pointer Capture steps for this PointerEvent.
    processPendingPointerCapture(event);

    // If the pointer capture target override has been set for the pointer, set the target to pointer capture target override object.
    auto iterator = m_activePointerIdsToCapturingData.find(event.pointerId());
    if (iterator != m_activePointerIdsToCapturingData.end()) {
        auto& capturingData = iterator->value;
        if (capturingData.targetOverride)
            target = capturingData.targetOverride.get();
    }

    pointerEventWillBeDispatched(event, target);
    target->dispatchEvent(event);
    pointerEventWasDispatched(event);
}

void PointerCaptureController::pointerEventWillBeDispatched(const PointerEvent& event, EventTarget* target)
{
    if (!is<Element>(target))
        return;

    bool isPointerdown = event.type() == eventNames().pointerdownEvent;
    bool isPointerup = event.type() == eventNames().pointerupEvent;
    if (!isPointerdown && !isPointerup)
        return;

    auto pointerId = event.pointerId();

    if (event.pointerType() == PointerEvent::mousePointerType()) {
        auto iterator = m_activePointerIdsToCapturingData.find(pointerId);
        if (iterator != m_activePointerIdsToCapturingData.end())
            iterator->value.pointerIsPressed = isPointerdown;
        return;
    }

    if (!isPointerdown)
        return;

    // https://w3c.github.io/pointerevents/#implicit-pointer-capture

    // Some input devices (such as touchscreens) implement a "direct manipulation" metaphor where a pointer is intended to act primarily on the UI
    // element it became active upon (providing a physical illusion of direct contact, instead of indirect contact via a cursor that conceptually
    // floats above the UI). Such devices are identified by the InputDeviceCapabilities.pointerMovementScrolls property and should have "implicit
    // pointer capture" behavior as follows.

    // Direct manipulation devices should behave exactly as if setPointerCapture was called on the target element just before the invocation of any
    // pointerdown listeners. The hasPointerCapture API may be used (eg. within any pointerdown listener) to determine whether this has occurred. If
    // releasePointerCapture is not called for the pointer before the next pointer event is fired, then a gotpointercapture event will be dispatched
    // to the target (as normal) indicating that capture is active.

    CapturingData capturingData;
    capturingData.pointerType = event.pointerType();
    capturingData.pointerIsPressed = true;
    m_activePointerIdsToCapturingData.set(pointerId, capturingData);
    setPointerCapture(downcast<Element>(target), pointerId);
}

void PointerCaptureController::pointerEventWasDispatched(const PointerEvent& event)
{
    auto iterator = m_activePointerIdsToCapturingData.find(event.pointerId());
    if (iterator != m_activePointerIdsToCapturingData.end()) {
        auto& capturingData = iterator->value;
        capturingData.isPrimary = event.isPrimary();

        // Immediately after firing the pointerup or pointercancel events, a user agent MUST clear the pending pointer capture target
        // override for the pointerId of the pointerup or pointercancel event that was just dispatched, and then run Process Pending
        // Pointer Capture steps to fire lostpointercapture if necessary.
        // https://w3c.github.io/pointerevents/#implicit-release-of-pointer-capture
        if (event.type() == eventNames().pointerupEvent) {
            capturingData.pendingTargetOverride = nullptr;
            processPendingPointerCapture(event);
        }

        // If a mouse pointer has moved while it isn't pressed, make sure we reset the preventsCompatibilityMouseEvents flag since
        // we could otherwise prevent compatibility mouse events while those are only supposed to be prevented while the pointer is pressed.
        if (event.type() == eventNames().pointermoveEvent && capturingData.pointerType == PointerEvent::mousePointerType() && !capturingData.pointerIsPressed)
            capturingData.preventsCompatibilityMouseEvents = false;

        // If the pointer event dispatched was pointerdown and the event was canceled, then set the PREVENT MOUSE EVENT flag for this pointerType.
        // https://www.w3.org/TR/pointerevents/#mapping-for-devices-that-support-hover
        if (event.type() == eventNames().pointerdownEvent)
            capturingData.preventsCompatibilityMouseEvents = event.defaultPrevented();
    }
}

void PointerCaptureController::cancelPointer(PointerID pointerId, const IntPoint& documentPoint)
{
    // https://w3c.github.io/pointerevents/#the-pointercancel-event

    // A user agent MUST fire a pointer event named pointercancel in the following circumstances:
    //
    // The user agent has determined that a pointer is unlikely to continue to produce events (for example, because of a hardware event).
    // After having fired the pointerdown event, if the pointer is subsequently used to manipulate the page viewport (e.g. panning or zooming).
    // Immediately before drag operation starts [HTML], for the pointer that caused the drag operation.
    // After firing the pointercancel event, a user agent MUST also fire a pointer event named pointerout followed by firing a pointer event named pointerleave.

    // https://w3c.github.io/pointerevents/#implicit-release-of-pointer-capture

    // Immediately after firing the pointerup or pointercancel events, a user agent MUST clear the pending pointer capture target
    // override for the pointerId of the pointerup or pointercancel event that was just dispatched, and then run Process Pending
    // Pointer Capture steps to fire lostpointercapture if necessary. After running Process Pending Pointer Capture steps, if the
    // pointer supports hover, user agent MUST also send corresponding boundary events necessary to reflect the current position of
    // the pointer with no capture.

    auto iterator = m_activePointerIdsToCapturingData.find(pointerId);
    if (iterator == m_activePointerIdsToCapturingData.end())
        return;

    auto& capturingData = iterator->value;
    if (capturingData.cancelled)
        return;

    capturingData.pendingTargetOverride = nullptr;
    capturingData.cancelled = true;

    auto& target = capturingData.targetOverride;
    if (!target)
        target = m_page.mainFrame().eventHandler().hitTestResultAtPoint(documentPoint, HitTestRequest::ReadOnly | HitTestRequest::Active | HitTestRequest::DisallowUserAgentShadowContent | HitTestRequest::AllowChildFrameContent).innerNonSharedElement();

    if (!target)
        return;

    // After firing the pointercancel event, a user agent MUST also fire a pointer event named pointerout
    // followed by firing a pointer event named pointerleave.
    auto isPrimary = capturingData.isPrimary ? PointerEvent::IsPrimary::Yes : PointerEvent::IsPrimary::No;
    auto cancelEvent = PointerEvent::create(eventNames().pointercancelEvent, pointerId, capturingData.pointerType, isPrimary);
    target->dispatchEvent(cancelEvent);
    target->dispatchEvent(PointerEvent::create(eventNames().pointeroutEvent, pointerId, capturingData.pointerType, isPrimary));
    target->dispatchEvent(PointerEvent::create(eventNames().pointerleaveEvent, pointerId, capturingData.pointerType, isPrimary));
    processPendingPointerCapture(WTFMove(cancelEvent));
}

void PointerCaptureController::processPendingPointerCapture(const PointerEvent& event)
{
    // https://w3c.github.io/pointerevents/#process-pending-pointer-capture

    auto iterator = m_activePointerIdsToCapturingData.find(event.pointerId());
    if (iterator == m_activePointerIdsToCapturingData.end())
        return;

    auto& capturingData = iterator->value;

    // Cache the pending target override since it could be modified during the dispatch of events in this function.
    auto pendingTargetOverride = capturingData.pendingTargetOverride;

    // 1. If the pointer capture target override for this pointer is set and is not equal to the pending pointer capture target override,
    // then fire a pointer event named lostpointercapture at the pointer capture target override node.
    if (capturingData.targetOverride && capturingData.targetOverride->isConnected() && capturingData.targetOverride != pendingTargetOverride)
        capturingData.targetOverride->dispatchEvent(PointerEvent::createForPointerCapture(eventNames().lostpointercaptureEvent, event));

    // 2. If the pending pointer capture target override for this pointer is set and is not equal to the pointer capture target override,
    // then fire a pointer event named gotpointercapture at the pending pointer capture target override.
    if (capturingData.pendingTargetOverride && capturingData.targetOverride != pendingTargetOverride)
        pendingTargetOverride->dispatchEvent(PointerEvent::createForPointerCapture(eventNames().gotpointercaptureEvent, event));

    // 3. Set the pointer capture target override to the pending pointer capture target override, if set. Otherwise, clear the pointer
    // capture target override.
    capturingData.targetOverride = pendingTargetOverride;
}

} // namespace WebCore

#endif // ENABLE(POINTER_EVENTS)
