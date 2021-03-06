/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2016 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "TrackMarker.h"

#include <QtCore/QCoreApplication>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/Curve.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/AppManager.h"
#include "Engine/AppInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageComponents.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/TrackerNode.h"
#include "Engine/TrackerNodePrivate.h"
#include "Engine/TimeLine.h"
#include "Engine/TLSHolder.h"

#include "Serialization/KnobTableItemSerialization.h"


#include <ofxNatron.h>

#define kTrackerPMParamScore "score"
#define kTrackerPMParamTrackingNext kNatronParamTrackingNext
#define kTrackerPMParamTrackingPrevious kNatronParamTrackingPrevious
#define kTrackerPMParamTrackingSearchBoxTopRight "searchBoxTopRight"
#define kTrackerPMParamTrackingSearchBoxBtmLeft "searchBoxBtmLeft"
#define kTrackerPMParamTrackingPatternBoxTopRight "patternBoxTopRight"
#define kTrackerPMParamTrackingPatternBoxBtmLeft "patternBoxBtmLeft"
#define kTrackerPMParamTrackingCorrelationScore "correlation"
#define kTrackerPMParamTrackingReferenceFrame "refFrame"
#define kTrackerPMParamTrackingEnableReferenceFrame "enableRefFrame"
#define kTrackerPMParamTrackingOffset "offset"
#define kTrackerPMParamTrackingCenterPoint "center"


NATRON_NAMESPACE_ENTER;


NATRON_NAMESPACE_ANONYMOUS_ENTER

class MetaTypesRegistration
{
public:
    inline MetaTypesRegistration()
    {
        qRegisterMetaType<TrackMarkerPtr>("TrackMarkerPtr");
    }
};

NATRON_NAMESPACE_ANONYMOUS_EXIT

static MetaTypesRegistration registration;

struct TrackMarkerPrivate
{
    TrackMarker* _publicInterface; // can not be a smart ptr

    // Defines the rectangle of the search window, this is in coordinates relative to the marker center point
    KnobDoubleWPtr searchWindowBtmLeft, searchWindowTopRight;

    // The pattern Quad defined by 4 corners relative to the center
    KnobDoubleWPtr patternTopLeft, patternTopRight, patternBtmRight, patternBtmLeft;
    KnobDoubleWPtr center, offset, error;
#ifdef NATRON_TRACK_MARKER_USE_WEIGHT
    KnobDoubleWPtr weight;
#endif
    KnobChoiceWPtr motionModel;
    mutable QMutex trackMutex;
    KnobBoolWPtr enabled;

    // Only used by the TrackScheduler thread
    int trackingStartedCount;
    std::list<double> keyframesAddedWhileTracking;

    TrackMarkerPrivate(TrackMarker* publicInterface)
        : _publicInterface(publicInterface)
        , searchWindowBtmLeft()
        , searchWindowTopRight()
        , patternTopLeft()
        , patternTopRight()
        , patternBtmRight()
        , patternBtmLeft()
        , center()
        , offset()
        , error()
#ifdef NATRON_TRACK_MARKER_USE_WEIGHT
        , weight()
#endif
        , motionModel()
        , trackMutex()
        , enabled()
        , trackingStartedCount(0)
        , keyframesAddedWhileTracking()
    {
    }
};

TrackMarker::TrackMarker(const KnobItemsTablePtr& model)
    : KnobTableItem(model)
    , _imp( new TrackMarkerPrivate(this) )
{
}

TrackMarker::~TrackMarker()
{
}

std::string
TrackMarker::getBaseItemName() const
{
    return tr("Track").toStdString();
}

std::string
TrackMarker::getSerializationClassName() const
{
    return kSerializationTrackTag;
}

void
TrackMarker::initializeKnobs()
{
    KnobItemsTablePtr model = getModel();
    EffectInstancePtr effect;
    if (model) {
        effect = model->getNode()->getEffectInstance();
    }
    KnobIntPtr defPatternSizeKnob, defSearchSizeKnob;
    KnobChoicePtr defMotionModelKnob;
    defPatternSizeKnob = toKnobInt(effect->getKnobByName(kTrackerUIParamDefaultMarkerPatternWinSize));
    defSearchSizeKnob = toKnobInt(effect->getKnobByName(kTrackerUIParamDefaultMarkerSearchWinSize));
    defMotionModelKnob = toKnobChoice(effect->getKnobByName(kTrackerUIParamDefaultMotionModel));

    double patternHalfSize = defPatternSizeKnob ? defPatternSizeKnob->getValue() / 2. : 21;
    double searchHalfSize = defSearchSizeKnob ? defSearchSizeKnob->getValue() / 2. : 71;

    int defMotionModel_i = defMotionModelKnob ? defMotionModelKnob->getValue() : 0;

    KnobDoublePtr swbbtmLeft = AppManager::createKnob<KnobDouble>(shared_from_this(), tr(kTrackerParamSearchWndBtmLeftLabel), 2, false);

    swbbtmLeft->setName(kTrackerParamSearchWndBtmLeft);
    swbbtmLeft->setDefaultValue(-searchHalfSize, DimIdx(0));
    swbbtmLeft->setDefaultValue(-searchHalfSize, DimIdx(1));
    swbbtmLeft->setHintToolTip( tr(kTrackerParamSearchWndBtmLeftHint) );
    _imp->searchWindowBtmLeft = swbbtmLeft;

    KnobDoublePtr swbtRight = AppManager::createKnob<KnobDouble>(shared_from_this(), tr(kTrackerParamSearchWndTopRightLabel), 2, false);
    swbtRight->setName(kTrackerParamSearchWndTopRight);
    swbtRight->setDefaultValue(searchHalfSize, DimIdx(0));
    swbtRight->setDefaultValue(searchHalfSize, DimIdx(1));
    swbtRight->setHintToolTip( tr(kTrackerParamSearchWndTopRightHint) );
    _imp->searchWindowTopRight = swbtRight;


    KnobDoublePtr ptLeft = AppManager::createKnob<KnobDouble>(shared_from_this(), tr(kTrackerParamPatternTopLeftLabel), 2, false);
    ptLeft->setName(kTrackerParamPatternTopLeft);
    ptLeft->setDefaultValue(-patternHalfSize, DimIdx(0));
    ptLeft->setDefaultValue(patternHalfSize, DimIdx(1));
    ptLeft->setHintToolTip( tr(kTrackerParamPatternTopLeftHint) );
    _imp->patternTopLeft = ptLeft;

    KnobDoublePtr ptRight = AppManager::createKnob<KnobDouble>(shared_from_this(), tr(kTrackerParamPatternTopRightLabel), 2, false);
    ptRight->setName(kTrackerParamPatternTopRight);
    ptRight->setDefaultValue(patternHalfSize, DimIdx(0));
    ptRight->setDefaultValue(patternHalfSize, DimIdx(1));
    ptRight->setHintToolTip( tr(kTrackerParamPatternTopRightHint) );
    _imp->patternTopRight = ptRight;

    KnobDoublePtr pBRight = AppManager::createKnob<KnobDouble>(shared_from_this(), tr(kTrackerParamPatternBtmRightLabel), 2, false);
    pBRight->setName(kTrackerParamPatternBtmRight);
    pBRight->setDefaultValue(patternHalfSize, DimIdx(0));
    pBRight->setDefaultValue(-patternHalfSize, DimIdx(1));
    pBRight->setHintToolTip( tr(kTrackerParamPatternBtmRightHint) );
    _imp->patternBtmRight = pBRight;

    KnobDoublePtr pBLeft = AppManager::createKnob<KnobDouble>(shared_from_this(), tr(kTrackerParamPatternBtmLeftLabel), 2, false);
    pBLeft->setName(kTrackerParamPatternBtmLeft);
    pBLeft->setDefaultValue(-patternHalfSize, DimIdx(0));
    pBLeft->setDefaultValue(-patternHalfSize, DimIdx(1));
    pBLeft->setHintToolTip( tr(kTrackerParamPatternBtmLeftHint) );
    _imp->patternBtmLeft = pBLeft;

    KnobDoublePtr centerKnob = AppManager::createKnob<KnobDouble>(shared_from_this(), tr(kTrackerParamCenterLabel), 2, false);
    centerKnob->setName(kTrackerParamCenter);
    centerKnob->setHintToolTip( tr(kTrackerParamCenterHint) );
    _imp->center = centerKnob;

    KnobDoublePtr offsetKnob = AppManager::createKnob<KnobDouble>(shared_from_this(), tr(kTrackerParamOffsetLabel), 2, false);
    offsetKnob->setName(kTrackerParamOffset);
    offsetKnob->setHintToolTip( tr(kTrackerParamOffsetHint) );
    _imp->offset = offsetKnob;

#ifdef NATRON_TRACK_MARKER_USE_WEIGHT
    KnobDoublePtr weightKnob = AppManager::createKnob<KnobDouble>(shared_from_this(), tr(kTrackerParamTrackWeightLabel), 1, false);
    weightKnob->setName(kTrackerParamTrackWeight);
    weightKnob->setHintToolTip( tr(kTrackerParamTrackWeightHint) );
    weightKnob->setDefaultValue(1.);
    weightKnob->setAnimationEnabled(false);
    weightKnob->setRange(0., 1.);
    _imp->weight = weightKnob;
#endif

    KnobChoicePtr mmodelKnob = AppManager::createKnob<KnobChoice>(shared_from_this(), tr(kTrackerParamMotionModelLabel), 1, false);
    mmodelKnob->setHintToolTip( tr(kTrackerParamMotionModelHint) );
    mmodelKnob->setName(kTrackerParamMotionModel);
    {
        std::vector<std::string> choices, helps;
        std::map<int, std::string> icons;
        TrackerNodePrivate::getMotionModelsAndHelps(true, &choices, &helps, &icons);
        mmodelKnob->populateChoices(choices, helps);
        mmodelKnob->setIcons(icons);
    }

    mmodelKnob->setDefaultValue(defMotionModel_i);
    _imp->motionModel = mmodelKnob;

    KnobDoublePtr errKnob = AppManager::createKnob<KnobDouble>(shared_from_this(), tr(kTrackerParamErrorLabel), 1, false);
    errKnob->setName(kTrackerParamError);
    _imp->error = errKnob;

    KnobBoolPtr enableKnob = AppManager::createKnob<KnobBool>(shared_from_this(), tr(kTrackerParamEnabledLabel), 1, false);
    enableKnob->setName(kTrackerParamEnabled);
    enableKnob->setHintToolTip( tr(kTrackerParamEnabledHint) );
    enableKnob->setAnimationEnabled(true);
    enableKnob->setDefaultValue(true);
    _imp->enabled = enableKnob;

    addColumn(kKnobTableItemColumnLabel, DimIdx(0));
    addColumn(kTrackerParamEnabled, DimIdx(0));
    addColumn(kTrackerParamMotionModel, DimIdx(0));
    addColumn(kTrackerParamCenter, DimIdx(0));
    addColumn(kTrackerParamCenter, DimIdx(1));
    addColumn(kTrackerParamOffset, DimIdx(0));
    addColumn(kTrackerParamOffset, DimIdx(1));
    addColumn(kTrackerParamError, DimIdx(0));

} // TrackMarker::initializeKnobs

void
TrackMarker::copyItem(const KnobTableItemPtr& other)
{
    TrackMarkerPtr thisShared = shared_from_this();

    TrackMarkerPtr otherMarker = toTrackMarker(other);
    if (!otherMarker) {
        return;
    }

    KnobTableItem::copyItem(other);
}



KnobDoublePtr
TrackMarker::getSearchWindowBottomLeftKnob() const
{
    return _imp->searchWindowBtmLeft.lock();
}

KnobDoublePtr
TrackMarker::getSearchWindowTopRightKnob() const
{
    return _imp->searchWindowTopRight.lock();
}

KnobDoublePtr
TrackMarker::getPatternTopLeftKnob() const
{
    return _imp->patternTopLeft.lock();
}

KnobDoublePtr
TrackMarker::getPatternTopRightKnob() const
{
    return _imp->patternTopRight.lock();
}

KnobDoublePtr
TrackMarker::getPatternBtmRightKnob() const
{
    return _imp->patternBtmRight.lock();
}

KnobDoublePtr
TrackMarker::getPatternBtmLeftKnob() const
{
    return _imp->patternBtmLeft.lock();
}

#ifdef NATRON_TRACK_MARKER_USE_WEIGHT
KnobDoublePtr
TrackMarker::getWeightKnob() const
{
    return _imp->weight.lock();
}

#endif

KnobDoublePtr
TrackMarker::getCenterKnob() const
{
    return _imp->center.lock();
}

KnobDoublePtr
TrackMarker::getOffsetKnob() const
{
    return _imp->offset.lock();
}

KnobDoublePtr
TrackMarker::getErrorKnob() const
{
    return _imp->error.lock();
}

KnobChoicePtr
TrackMarker::getMotionModelKnob() const
{
    return _imp->motionModel.lock();
}

KnobBoolPtr
TrackMarker::getEnabledKnob() const
{
    return _imp->enabled.lock();
}


void
TrackMarker::getCenterKeyframes(std::set<double>* keyframes) const
{
    CurvePtr curve = _imp->center.lock()->getCurve(ViewGetSpec(0), DimIdx(0));

    assert(curve);
    KeyFrameSet keys = curve->getKeyFrames_mt_safe();
    for (KeyFrameSet::iterator it = keys.begin(); it != keys.end(); ++it) {
        keyframes->insert( it->getTime() );
    }
}

bool
TrackMarker::isEnabled(double time) const
{
    return _imp->enabled.lock()->getValueAtTime(time, DimIdx(0), ViewGetSpec::current(), true, true /*byPassMaster*/);
}

void
TrackMarker::setEnabledAtTime(double time,
                              bool enabled)
{
    KnobBoolPtr knob = _imp->enabled.lock();

    if (!knob) {
        return;
    }
    knob->setValueAtTime(time, enabled, ViewSetSpec::all(), DimIdx(0));
}

AnimationLevelEnum
TrackMarker::getEnabledNessAnimationLevel() const
{
    return _imp->enabled.lock()->getAnimationLevel(DimIdx(0), ViewIdx(0));
}

int
TrackMarker::getReferenceFrame(double time,
                               int frameStep) const
{
    QMutexLocker k(&_imp->trackMutex);

    std::set<double> userKeyframes;
    getMasterKeyFrameTimes(ViewGetSpec(0), &userKeyframes);

    std::set<double>::iterator upper = userKeyframes.upper_bound(time);

    if ( upper == userKeyframes.end() ) {
        //all keys are lower than time, pick the last one
        if ( !userKeyframes.empty() ) {
            return *userKeyframes.rbegin();
        }

        // no keyframe - use the previous/next as reference
        return time - frameStep;
    } else {
        if ( upper == userKeyframes.begin() ) {
            ///all keys are greater than time
            return *upper;
        }

        int upperKeyFrame = *upper;
        ///If we find "time" as a keyframe, then use it
        --upper;

        int lowerKeyFrame = *upper;

        if (lowerKeyFrame == time) {
            return time;
        }

        /// return the nearest from time
        return (time - lowerKeyFrame) < (upperKeyFrame - time) ? lowerKeyFrame : upperKeyFrame;
    }
}

void
TrackMarker::resetCenter()
{
    KnobItemsTablePtr model = getModel();
    if (!model) {
        return;
    }
    RectD rod;
    NodePtr input = model->getNode()->getInput(0);
    if (!input) {
        Format f;
        getApp()->getProject()->getProjectDefaultFormat(&f);
        rod = f.toCanonicalFormat();
    } else {
        SequenceTime time = input->getApp()->getTimeLine()->currentFrame();
        RenderScale scale;
        scale.x = scale.y = 1;
        U64 inputHash;
        bool gotHash = input->getEffectInstance()->getRenderHash(time, ViewIdx(0), &inputHash);
        (void)gotHash;
        StatusEnum stat = input->getEffectInstance()->getRegionOfDefinition_public(inputHash, time, scale, ViewIdx(0), &rod);
        Point center;
        center.x = 0;
        center.y = 0;
        if (stat == eStatusOK) {
            center.x = (rod.x1 + rod.x2) / 2.;
            center.y = (rod.y1 + rod.y2) / 2.;
        }

        KnobDoublePtr centerKnob = getCenterKnob();
        centerKnob->setValue(center.x, ViewSetSpec::current(), DimIdx(0));
        centerKnob->setValue(center.y, ViewSetSpec::current(), DimIdx(1));
    }
}

enum DeleteKnobAnimationEnum
{
    eDeleteKnobAnimationAll,
    eDeleteKnobAnimationBeforeTime,
    eDeleteKnobAnimationAfterTime
};

static void
deleteKnobAnimation(const std::set<double>& userKeyframes,
                    const KnobIPtr& knob,
                    DeleteKnobAnimationEnum type,
                    double currentTime)
{
    for (int i = 0; i < knob->getNDimensions(); ++i) {
        CurvePtr curve = knob->getCurve(ViewGetSpec(0), DimIdx(i));
        assert(curve);
        KeyFrameSet keys = curve->getKeyFrames_mt_safe();
        std::list<double> toRemove;
        switch (type) {
        case eDeleteKnobAnimationAll: {
            for (KeyFrameSet::iterator it = keys.begin(); it != keys.end(); ++it) {
                std::set<double>::iterator found = userKeyframes.find( it->getTime() );
                if ( found == userKeyframes.end() ) {
                    toRemove.push_back( it->getTime() );
                }
            }
            break;
        }
        case eDeleteKnobAnimationBeforeTime: {
            for (KeyFrameSet::iterator it = keys.begin(); it != keys.end(); ++it) {
                if (it->getTime() >= currentTime) {
                    break;
                }
                std::set<double>::iterator found = userKeyframes.find( it->getTime() );
                if ( found == userKeyframes.end() ) {
                    toRemove.push_back( it->getTime() );
                }
            }
            break;
        }
        case eDeleteKnobAnimationAfterTime: {
            for (KeyFrameSet::reverse_iterator it = keys.rbegin(); it != keys.rend(); ++it) {
                if (it->getTime() <= currentTime) {
                    break;
                }
                std::set<double>::iterator found = userKeyframes.find( it->getTime() );
                if ( found == userKeyframes.end() ) {
                    toRemove.push_back( it->getTime() );
                }
            }
            break;
        }
        }
        knob->deleteValuesAtTime(toRemove, ViewSetSpec::all(), DimIdx(i));
    }
}

void
TrackMarker::clearAnimation()
{
    std::set<double> userKeyframes;

    getMasterKeyFrameTimes(ViewGetSpec(0), &userKeyframes);

    KnobIPtr offsetKnob = getOffsetKnob();
    assert(offsetKnob);
    deleteKnobAnimation(userKeyframes, offsetKnob, eDeleteKnobAnimationAll, 0);

    KnobIPtr centerKnob = getCenterKnob();
    assert(centerKnob);
    deleteKnobAnimation(userKeyframes, centerKnob, eDeleteKnobAnimationAll, 0);

    KnobIPtr errorKnob = getErrorKnob();
    assert(errorKnob);
    deleteKnobAnimation(userKeyframes, errorKnob, eDeleteKnobAnimationAll, 0);
}

void
TrackMarker::clearAnimationBeforeTime(double time)
{
    std::set<double> userKeyframes;

    getMasterKeyFrameTimes(ViewGetSpec(0), &userKeyframes);

    KnobIPtr offsetKnob = getOffsetKnob();
    assert(offsetKnob);
    deleteKnobAnimation(userKeyframes, offsetKnob, eDeleteKnobAnimationBeforeTime, time);

    KnobIPtr centerKnob = getCenterKnob();
    assert(centerKnob);
    deleteKnobAnimation(userKeyframes, centerKnob, eDeleteKnobAnimationBeforeTime, time);

    KnobIPtr errorKnob = getErrorKnob();
    assert(errorKnob);
    deleteKnobAnimation(userKeyframes, errorKnob, eDeleteKnobAnimationBeforeTime, time);
}

void
TrackMarker::clearAnimationAfterTime(double time)
{
    std::set<double> userKeyframes;

    getMasterKeyFrameTimes(ViewGetSpec(0), &userKeyframes);

    KnobIPtr offsetKnob = getOffsetKnob();
    assert(offsetKnob);
    deleteKnobAnimation(userKeyframes, offsetKnob, eDeleteKnobAnimationAfterTime, time);

    KnobIPtr centerKnob = getCenterKnob();
    assert(centerKnob);
    deleteKnobAnimation(userKeyframes, centerKnob, eDeleteKnobAnimationAfterTime, time);

    KnobIPtr errorKnob = getErrorKnob();
    assert(errorKnob);
    deleteKnobAnimation(userKeyframes, errorKnob, eDeleteKnobAnimationAfterTime, time);
}

void
TrackMarker::resetOffset()
{
    KnobDoublePtr knob = getOffsetKnob();
    knob->resetToDefaultValue(DimSpec::all(), ViewSetSpec::all());
}

void
TrackMarker::resetTrack()
{
    Point curCenter;
    KnobDoublePtr centerKnob = getCenterKnob();

    curCenter.x = centerKnob->getValue();
    curCenter.y = centerKnob->getValue(DimIdx(1));

    const KnobsVec& knobs = getKnobs();
    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        if (*it != centerKnob) {
            (*it)->resetToDefaultValue(DimSpec::all(), ViewSetSpec::all());
        } else {
            (*it)->removeAnimation(ViewSetSpec::all(), DimSpec::all());

            std::vector<double> values(2);
            values[0] = curCenter.x;
            values[1] = curCenter.y;
            centerKnob->setValueAcrossDimensions(values);
        }
    }

    removeAnimation(ViewSetSpec::all(), DimSpec::all());
}


void
TrackMarker::setKeyFrameOnCenterAndPatternAtTime(double time)
{
    KnobDoublePtr center = _imp->center.lock();

    {
        std::vector<double> values(2);
        values[0] = center->getValueAtTime(time);
        values[1] = center->getValueAtTime(time, DimIdx(1));
        center->setValueAtTimeAcrossDimensions(time, values);
    }

    KnobDoublePtr patternCorners[4] = {_imp->patternBtmLeft.lock(), _imp->patternTopLeft.lock(), _imp->patternTopRight.lock(), _imp->patternBtmRight.lock()};
    for (int c = 0; c < 4; ++c) {
        KnobDoublePtr k = patternCorners[c];
        std::vector<double> values(2);
        values[0] = k->getValueAtTime(time, DimIdx(0));
        values[1] = k->getValueAtTime(time, DimIdx(1));
        k->setValueAcrossDimensions(values);
    }
}
void
TrackMarker::notifyTrackingStarted()
{
    ++_imp->trackingStartedCount;
    _imp->keyframesAddedWhileTracking.clear();
}

void
TrackMarker::notifyTrackingEnded()
{
    if (_imp->trackingStartedCount) {
        --_imp->trackingStartedCount;
    }

    // Refresh knobs once finished
    if (!_imp->trackingStartedCount) {
#pragma message WARN("Handle keyframes for trackmarker center knob")
        /*TrackMarkerPtr marker = shared_from_this();
        getContext()->s_errorKnobValueChanged(marker, 0, eValueChangedReasonNatronInternalEdited);
        for (int i = 0; i < 2; ++i) {
            getContext()->s_centerKnobValueChanged(marker, i, eValueChangedReasonNatronInternalEdited);
        }
        if (!_imp->keyframesAddedWhileTracking.empty()) {
            getContext()->s_multipleKeyframesSetOnTrackCenter(marker, _imp->keyframesAddedWhileTracking);
            _imp->keyframesAddedWhileTracking.clear();
        }*/
    }
}


RectI
TrackMarker::getMarkerImageRoI(double time) const
{
    const unsigned int mipmapLevel = 0;
    Point center, offset;
    KnobDoublePtr centerKnob = getCenterKnob();
    KnobDoublePtr offsetKnob = getOffsetKnob();

    center.x = centerKnob->getValueAtTime(time, DimIdx(0));
    center.y = centerKnob->getValueAtTime(time, DimIdx(1));

    offset.x = offsetKnob->getValueAtTime(time, DimIdx(0));
    offset.y = offsetKnob->getValueAtTime(time, DimIdx(1));

    RectD roiCanonical;
    KnobDoublePtr swBl = getSearchWindowBottomLeftKnob();
    KnobDoublePtr swTr = getSearchWindowTopRightKnob();

    roiCanonical.x1 = swBl->getValueAtTime(time, DimIdx(0)) + center.x + offset.x;
    roiCanonical.y1 = swBl->getValueAtTime(time, DimIdx(1)) + center.y + offset.y;
    roiCanonical.x2 = swTr->getValueAtTime(time, DimIdx(0)) + center.x + offset.x;
    roiCanonical.y2 = swTr->getValueAtTime(time, DimIdx(1)) + center.y + offset.y;

    RectI roi;
    NodePtr node = getModel()->getNode();
    NodePtr input = node->getInput(0);
    if (!input) {
        return RectI();
    }
    roiCanonical.toPixelEnclosing(mipmapLevel, input ? input->getEffectInstance()->getAspectRatio(-1) : 1., &roi);

    return roi;
}

std::pair<ImagePtr, RectI>
TrackMarker::getMarkerImage(double time,
                            const RectI& roi) const
{
    std::list<ImageComponents> components;

    components.push_back( ImageComponents::getRGBComponents() );

    const unsigned int mipmapLevel = 0;
    assert( !roi.isNull() );

    NodePtr node = getModel()->getNode();
    NodePtr input = node->getInput(0);
    if (!input) {
        return std::make_pair(ImagePtr(), roi);
    }

    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    const bool isRenderUserInteraction = true;
    const bool isSequentialRender = false;
    AbortableThread* isAbortable = dynamic_cast<AbortableThread*>( QThread::currentThread() );
    if (isAbortable) {
        isAbortable->setAbortInfo( isRenderUserInteraction, abortInfo, node->getEffectInstance() );
    }

    ParallelRenderArgsSetter::CtorArgsPtr tlsArgs(new ParallelRenderArgsSetter::CtorArgs);
    tlsArgs->time = time;
    tlsArgs->view = ViewIdx(0);
    tlsArgs->isRenderUserInteraction = isRenderUserInteraction;
    tlsArgs->isSequential = isSequentialRender;
    tlsArgs->abortInfo = abortInfo;
    tlsArgs->treeRoot = getModel()->getNode();
    tlsArgs->textureIndex = 0;
    tlsArgs->timeline = node->getApp()->getTimeLine();
    tlsArgs->activeRotoPaintNode = NodePtr();
    tlsArgs->activeRotoDrawableItem = RotoDrawableItemPtr();
    tlsArgs->isDoingRotoNeatRender = false;
    tlsArgs->isAnalysis = true;
    tlsArgs->draftMode = true;
    tlsArgs->stats = RenderStatsPtr();
    boost::shared_ptr<ParallelRenderArgsSetter> frameRenderArgs;
    try {
        frameRenderArgs.reset(new ParallelRenderArgsSetter(tlsArgs));
    } catch (...) {
        return std::make_pair(ImagePtr(), roi);
    }

    EffectInstancePtr effectToRender = input->getEffectInstance();

    U64 effectHash;
    bool gotHash = effectToRender->getRenderHash(time, ViewIdx(0), &effectHash);
    assert(gotHash);
    (void)gotHash;
    RenderScale scale;
    scale.x = scale.y = 1.;
    unsigned int mipMapLevel = 0;
    RectD precomputedRoD;
    double par = effectToRender->getAspectRatio(-1);

    {
        StatusEnum stat = effectToRender->getRegionOfDefinition_public(effectHash, time, scale, ViewIdx(0), &precomputedRoD);
        if (stat == eStatusFailed) {
            return std::make_pair(ImagePtr(), roi);
        }
    }

    RectD canonicalRoi;
    roi.toCanonical(mipMapLevel, par, precomputedRoD, &canonicalRoi);
    if (frameRenderArgs->computeRequestPass(mipmapLevel, canonicalRoi) != eStatusOK) {
        return std::make_pair(ImagePtr(), roi);
    }


    EffectInstance::RenderRoIArgs args( time,
                                        scale,
                                        mipmapLevel, //mipmaplevel
                                        ViewIdx(0),
                                        false,
                                        roi,
                                        RectD(),
                                        components,
                                        eImageBitDepthFloat,
                                        false,
                                        node->getEffectInstance(),
                                        eStorageModeRAM /*returnOpenGlTex*/,
                                        time);
    std::map<ImageComponents, ImagePtr> planes;
    EffectInstance::RenderRoIRetCode stat = effectToRender->renderRoI(args, &planes);

    appPTR->getAppTLS()->cleanupTLSForThread();

    if ( (stat != EffectInstance::eRenderRoIRetCodeOk) || planes.empty() ) {
        return std::make_pair(ImagePtr(), roi);
    }

    return std::make_pair(planes.begin()->second, roi);
} // TrackMarker::getMarkerImage



TrackMarkerPM::TrackMarkerPM(const KnobItemsTablePtr& context)
    : TrackMarker(context)
{
}

TrackMarkerPM::~TrackMarkerPM()
{
}

void
TrackMarkerPM::onTrackerNodeInputChanged(int /*inputNb*/)
{
    NodePtr thisNode = getModel()->getNode();

    if (thisNode) {
        NodePtr inputNode = thisNode->getInput(0);
        if (inputNode) {
            trackerNode->connectInput(inputNode, 0);
        }
    }
}

bool
TrackMarkerPM::trackMarker(bool forward,
                           int refFrame,
                           int frame)
{
    KnobButtonPtr button;

    if (forward) {
        button = trackNextButton.lock();
    } else {
        button = trackPrevButton.lock();
    }
    KnobIntPtr refFrameK = refFrameKnob.lock();
    refFrameK->setValue(refFrame);

    // Unslave the center knob since the trackerNode will update it, then update the marker center
    KnobDoublePtr center = centerKnob.lock();
    center->unSlave(DimSpec::all(), ViewSetSpec::all(), true);

    trackerNode->getEffectInstance()->onKnobValueChanged_public(button, eValueChangedReasonNatronInternalEdited, frame, ViewIdx(0),
                                                                true);

    KnobDoublePtr markerCenter = getCenterKnob();
    // The TrackerPM plug-in has set a keyframe at the refFrame and frame, copy them
    bool ret = true;
    double centerPoint[2];
    for (int i = 0; i < center->getNDimensions(); ++i) {
        {
            int index = center->getKeyFrameIndex(ViewGetSpec::current(), DimIdx(i), frame);
            if (index != -1) {
                centerPoint[i] = center->getValueAtTime(frame, DimIdx(i));
                markerCenter->setValueAtTime(frame, centerPoint[i], ViewSetSpec::current(), DimIdx(i));
            } else {
                // No keyframe at this time: tracking failed
                ret = false;
                break;
            }
        }
        {
            int index = center->getKeyFrameIndex(ViewGetSpec::current(), DimIdx(i), refFrame);
            if (index != -1) {
                double value = center->getValueAtTime(refFrame, DimIdx(i));
                markerCenter->setValueAtTime(refFrame, value, ViewSetSpec::current(), DimIdx(i));
            }
        }
    }

    // Convert the correlation score of the TrackerPM to the error
    if (ret) {
        KnobDoublePtr markerError = getErrorKnob();
        KnobDoublePtr correlation = correlationScoreKnob.lock();
        {
            int index = correlation->getKeyFrameIndex(ViewGetSpec::current(), DimIdx(0), frame);
            if (index != -1) {
                // The error is estimated as a percentage of the correlation across the number of pixels in the pattern window
                KnobDoublePtr  pBtmLeft = patternBtmLeftKnob.lock();
                KnobDoublePtr  pTopRight = patternTopRightKnob.lock();
                Point btmLeft, topRight;

                btmLeft.x = pBtmLeft->getValueAtTime(frame, DimIdx(0));
                btmLeft.y = pBtmLeft->getValueAtTime(frame, DimIdx(1));

                topRight.x = pTopRight->getValueAtTime(frame, DimIdx(0));
                topRight.y = pTopRight->getValueAtTime(frame, DimIdx(1));


                double areaPixels = (topRight.x - btmLeft.x) * (topRight.y - btmLeft.y);
                NodePtr trackerInput = trackerNode->getInput(0);
                if (trackerInput) {
                    ImageComponents comps = trackerInput->getEffectInstance()->getComponents(-1);
                    areaPixels *= comps.getNumComponents();
                }

                double value = correlation->getValueAtTime(frame, DimIdx(0));

                // Convert to a percentage
                value /= areaPixels;

                markerError->setValueAtTime(frame, value, ViewSetSpec::current(), DimIdx(0));
            }
        }
    }

    center->slaveTo(markerCenter);

    return ret;
} // TrackMarkerPM::trackMarker

template <typename T>
boost::shared_ptr<T>
getNodeKnob(const NodePtr& node,
            const std::string& scriptName)
{
    KnobIPtr knob = node->getKnobByName(scriptName);

    assert(knob);
    if (!knob) {
        return boost::shared_ptr<T>();
    }
    boost::shared_ptr<T> ret = boost::dynamic_pointer_cast<T>(knob);
    assert(ret);

    return ret;
}

void
TrackMarkerPM::initializeKnobs()
{
    TrackMarker::initializeKnobs();
    NodePtr thisNode = getModel()->getNode();
    NodePtr node;
    {
        CreateNodeArgsPtr args(CreateNodeArgs::create( PLUGINID_OFX_TRACKERPM, NodeCollectionPtr() ));
        args->setProperty<bool>(kCreateNodeArgsPropVolatile, true);
        args->setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
        args->setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, "TrackerPMNode");

        node = getApp()->createNode(args);
        if (!node) {
            throw std::runtime_error("Couldn't create plug-in " PLUGINID_OFX_TRACKERPM);
        }
        if (thisNode) {
            NodePtr inputNode = thisNode->getInput(0);
            if (inputNode) {
                node->connectInput(inputNode, 0);
            }
        }
        trackerNode = node;
    }

    KnobItemsTablePtr model = getModel();
    EffectInstancePtr effect;
    if (model) {
        effect = model->getNode()->getEffectInstance();
    }

    trackPrevButton = getNodeKnob<KnobButton>(node, kTrackerPMParamTrackingPrevious);
    trackNextButton = getNodeKnob<KnobButton>(node, kTrackerPMParamTrackingNext);
    KnobDoublePtr center = getNodeKnob<KnobDouble>(node, kTrackerPMParamTrackingCenterPoint);
    centerKnob = center;

    // Slave the center knob and unslave when tracking
    center->slaveTo(getCenterKnob());

    KnobDoublePtr offset = getNodeKnob<KnobDouble>(node, kTrackerPMParamTrackingOffset);

    // Slave the offset knob
    offset->slaveTo(getOffsetKnob());

    offsetKnob = offset;

    // Ref frame is set for each
    refFrameKnob = getNodeKnob<KnobInt>(node, kTrackerPMParamTrackingReferenceFrame);

    // Enable reference frame
    KnobBoolPtr enableRefFrameKnob = getNodeKnob<KnobBool>(node, kTrackerPMParamTrackingEnableReferenceFrame);
    enableRefFrameKnob->setValue(true);

    KnobChoicePtr scoreType = getNodeKnob<KnobChoice>(node, kTrackerPMParamScore);
    if (effect) {
#ifdef kTrackerParamPatternMatchingScoreType
        KnobIPtr modelKnob = effect->getKnobByName(kTrackerParamPatternMatchingScoreType);
        if (modelKnob) {
            scoreType->slaveTo(modelKnob);
        }
#endif
    }

    scoreTypeKnob = scoreType;

    KnobDoublePtr correlationScore = getNodeKnob<KnobDouble>(node, kTrackerPMParamTrackingCorrelationScore);
    correlationScoreKnob = correlationScore;

    KnobDoublePtr patternBtmLeft = getNodeKnob<KnobDouble>(node, kTrackerPMParamTrackingPatternBoxBtmLeft);
    patternBtmLeftKnob = patternBtmLeft;

    // Slave the search window and pattern of the node to the parameters of the marker
    patternBtmLeft->slaveTo(getPatternBtmLeftKnob());

    KnobDoublePtr patternTopRight = getNodeKnob<KnobDouble>(node, kTrackerPMParamTrackingPatternBoxTopRight);
    patternTopRightKnob = patternTopRight;
    patternTopRight->slaveTo(getPatternTopRightKnob());

    KnobDoublePtr searchWindowBtmLeft = getNodeKnob<KnobDouble>(node, kTrackerPMParamTrackingSearchBoxBtmLeft);
    searchWindowBtmLeftKnob = searchWindowBtmLeft;
    searchWindowBtmLeft->slaveTo(getSearchWindowBottomLeftKnob());

    KnobDoublePtr searchWindowTopRight = getNodeKnob<KnobDouble>(node, kTrackerPMParamTrackingSearchBoxTopRight);
    searchWindowTopRightKnob = searchWindowTopRight;
    searchWindowTopRight->slaveTo(getSearchWindowTopRightKnob());

} // TrackMarkerPM::initializeKnobs


NATRON_NAMESPACE_EXIT;
NATRON_NAMESPACE_USING;
#include "moc_TrackMarker.cpp"
