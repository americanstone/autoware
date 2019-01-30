/*
 * MapBuilder2.cpp
 *
 *  Created on: Jul 26, 2018
 *      Author: sujiwo
 */

#include <exception>
#include <thread>
#include "MapBuilder2.h"
#include "Optimizer.h"
#include "ImageDatabase.h"
#include "Viewer.h"
#include "utilities.h"



using namespace std;
using namespace Eigen;


MapBuilder2::frameCallback defaultFrameCallback =
[&] (const InputFrame &f)
{};


MapBuilder2::MapBuilder2() :
	kfAnchor(0),
	inputCallback(defaultFrameCallback)
{
	cMap = new VMap();
//	imageView = new Viewer;
}


MapBuilder2::~MapBuilder2()
{
//	delete(imageView);
}


void
MapBuilder2::initialize (const InputFrame &f1, const InputFrame &f2)
{
	kfid k1 = cMap->createKeyFrame(f1.image, f1.position, f1.orientation, f1.cameraId, NULL, f1.sourceId, f1.tm);
	kfid k2 = cMap->createKeyFrame(f2.image, f2.position, f2.orientation, f2.cameraId, NULL, f2.sourceId, f2.tm);
	cMap->estimateStructure(k1, k2);
	kfAnchor = k1;
	ifrAnchor = f1;
}


void
MapBuilder2::track (const InputFrame &f)
{
	if (initialized==false)
		throw runtime_error("Map not initialized");

	kfid fId = cMap->createKeyFrame(f.image, f.position, f.orientation, f.cameraId, NULL, f.sourceId, f.tm);
	cMap->estimateAndTrack(kfAnchor, fId);

	cMap->keyframe(fId)->previousKeyframe = kfAnchor;

	// XXX: Decide when to move the anchor
	kfAnchor = fId;
	ifrAnchor = f;
}


void
MapBuilder2::input(const InputFrame &f)
{
	if (isNormalFrame(f)==false)
		return;

	double runTrans, runRot;

	if (initialized==false) {

		if (frame0.image.empty()) {
			frame0 = f;
			return;
		}

		else {
			f.getPose().displacement(frame0.getPose(), runTrans, runRot);
			if (runTrans>=translationThrs or runRot>=rotationThrs) {
				initialize(frame0, f);
				inputCallback(f);
				initialized = true;
				cerr << "Initialized; # of map points: " << cMap->allMapPoints().size() << endl;
				return;
			}
		}
	}

	else {
		ifrAnchor.getPose().displacement(f.getPose(), runTrans, runRot);
		if (runTrans>=translationThrs or runRot>=rotationThrs) {
			kfid lastAnchor = kfAnchor;
			track (f);

			// Build connections
			vector<kfid> kfInsToAnchor = cMap->getKeyFramesComeInto(lastAnchor);
			cerr << "Found " << kfInsToAnchor.size() << " input keyframes\n";
			const kfid targetKfId = kfAnchor;

			// XXX: Parallelize this
			for (auto &kfx: kfInsToAnchor) {
				cMap->trackMapPoints(kfx, targetKfId);
			}
//			for (int i=0; i<min(4, int(kfInsToAnchor.size())); ++i) {
//				cMap->trackMapPoints(kfInsToAnchor[i], targetKfId);
//			}

			inputCallback(f);
		}
	}
}


/*
 * This function decides when a frame is `good enough' in terms of exposure
 * to be included for map building
 */
bool
MapBuilder2::isNormalFrame (const InputFrame &f)
{
	// throw away over-exposed frames
	// XXX: also need to do the same for under-exposed frame
	auto normcdf = cdf(f.image);
	if (normcdf[127] < 0.25)
		return false;
	else return true;
}


void
MapBuilder2::build ()
{
	mapPointCulling();
	cMap->fixFramePointsInv();

	thread ba([this] {
				cout << "Bundling...";
				bundle_adjustment(cMap);
//				bundle_adjustment_2(cMap);
				cout << "BA Done\n";
	});

	thread db([this] {
				cout << "Rebuilding Image DB... ";
				cout.flush();
				cMap->getImageDB()->rebuildAll();
				cout << "Image DB Build Done\n";
	});

	ba.join();
	db.join();
	return;
}


InputFrame createInputFrameX (GenericDataItem::ConstPtr DI)
{
	// We prefer gray images
	cv::Mat img=DI->getImage();
	cv::cvtColor(img, img, CV_BGR2GRAY, 1);

	InputFrame f(
		img,
		DI->getPosition(),
		DI->getOrientation(),
		DI->getId()
	);
	f.tm = DI->getTimestamp();

	return f;

}


void
MapBuilder2::runFromDataset(GenericDataset::Ptr sourceDs, const ptime startTime, const ptime stopTime)
{
	if (initialized != false)
		throw runtime_error("Map process has been running; aborted");

	if (startTime < sourceDs->first()->getTimestamp()
		or stopTime > sourceDs->last()->getTimestamp())
		throw runtime_error("Requested times are outside of dataset range");

	dataItemId
		startId = sourceDs->getLowerBound(startTime),
		stopId = sourceDs->getLowerBound(stopTime);

	return runFromDataset(sourceDs, startId, stopId);
}


void
MapBuilder2::runFromDataset(GenericDataset::Ptr sourceDs, dataItemId startPos, dataItemId stopPos)
{
	sourceDataset = sourceDs;
	cMap->reset();

	if (startPos==std::numeric_limits<dataItemId>::max() and
		stopPos==std::numeric_limits<dataItemId>::max()) {
		startPos = 0;
		stopPos = sourceDataset->size()-1;
	}

	for (auto currentId=startPos; currentId<=stopPos; ++currentId) {
		InputFrame cFrame = createInputFrameX (sourceDataset->get(currentId));
		this->input(cFrame);
	}

	this->build();

}


bool
MapBuilder2::checkDataPoints (GenericDataset::ConstPtr sourceDs, const ptime startTime, const ptime stopTime)
const
{
	dataItemId
		startId = sourceDs->getLowerBound(startTime),
		stopId = sourceDs->getLowerBound(stopTime);

	for (auto currentId=startId; currentId<=stopId; ++currentId) {
		auto curFrame = sourceDs->get(currentId);
		if (curFrame->getPose().isValid()==false)
			return false;
	}

	return true;
}


bool
MapBuilder2::checkDataPoints (GenericDataset::ConstPtr sourceDs, const dataItemId startPos, const dataItemId stopPos)
const
{
	for (auto currentId=startPos; currentId<=stopPos; ++currentId) {
		auto curFrame = sourceDs->get(currentId);
		if (curFrame->getPose().isValid()==false)
			return false;
	}

	return true;
}


void MapBuilder2::addCameraParam (const CameraPinholeParams &c)
{
	if (c.height==-1 or c.width==-1)
		throw runtime_error("Invalid camera parameter");

	cMap->addCameraParameter(c);
}


void
MapBuilder2::mapPointCulling()
{
	cout << "Culling points..." << flush;

	vector<mpid> mpToRemove;
	const int minKfRelatedFromMP = 3;
	auto allMapPoints = cMap->allMapPoints();
	const int N = allMapPoints.size();

	for (auto &mp: allMapPoints) {

		auto relatedKfs = cMap->getRelatedKeyFrames(mp);
		if (relatedKfs.size() < minKfRelatedFromMP) {
			mpToRemove.push_back(mp);
		}
	}

	cout << "Detected " << mpToRemove.size() << " points\n" << flush;

//	for (auto mp: mpToRemove) {
//		cMap->removeMapPoint(mp);
//	}
	cMap->removeMapPointsBatch(mpToRemove);

	cout << "Removed " << mpToRemove.size() << " out of " << N << " points" << flush;
}


void
MapBuilder2::setMask(const cv::Mat &m)
{
	mask = m.clone();
	cMap->setMask(mask);
}


void
MapBuilder2::simulateOpticalFlow(GenericDataset::ConstPtr sourceDs, dataItemId startPos, dataItemId stopPos)
{
	if (startPos==std::numeric_limits<dataItemId>::max() and stopPos==std::numeric_limits<dataItemId>::max()) {
		startPos = 0;
		stopPos = sourceDs->size()-1;
	}

	cMap->reset();

	// First keyframe
	auto frameItem = sourceDs->get(startPos);
	InputFrame kf1 = createInputFrameX(frameItem);

	for (dataItemId ix=startPos+1; ix<=stopPos; ix++) {
		// XXX: Unfinished
		InputFrame cInpFrame = createInputFrameX(sourceDs->get(ix));

	}
}


void
MapBuilder2::runFromDataset2
(GenericDataset::Ptr sourceDs, dataItemId startPos, dataItemId stopPos)
{

}


void
MapBuilder2::visualOdometry
(GenericDataset::Ptr sourceDs, dataItemId startPos, dataItemId stopPos)
{
	assert (0<=startPos and startPos<sourceDs->size()-1);
	assert (0<stopPos and stopPos<sourceDs->size());

	auto anchor = sourceDs->getAsFrame(startPos);

	for (dataItemId d=startPos+1; d<=stopPos; ++d) {
		auto curFrame = sourceDs->getAsFrame(d);

		// XXX: Unfinished
	}
}
