/*
 * collision_robot_fcl_derivatives.cpp
 *
 *  Created on: Dec 23, 2014
 *      Author: chonhyon
 */

#include <itomp_cio_planner/collision/collision_robot_fcl_derivatives.h>
#include <itomp_cio_planner/collision/collision_common_derivatives.h>
#include <ros/assert.h>

using namespace collision_detection;

namespace itomp_cio_planner
{

CollisionRobotFCLDerivatives::CollisionRobotFCLDerivatives(const CollisionRobotFCL &other)
	: CollisionRobotFCL(other)
{
    fcl::DynamicAABBTreeCollisionManager* m = new fcl::DynamicAABBTreeCollisionManager();
    manager_.manager_.reset(m);
}

void CollisionRobotFCLDerivatives::constructInternalFCLObject(const robot_state::RobotState &state)
{
    manager_.object_.clear();
    constructFCLObject(state, manager_.object_);

    manager_.manager_->clear();
    manager_.object_.registerTo(manager_.manager_.get());
}

void CollisionRobotFCLDerivatives::updateInternalFCLObjectTransforms(const robot_state::RobotState &state)
{
    FCLObject& fcl_obj = manager_.object_;

    std::size_t index = 0;
    for (std::size_t i = 0 ; i < geoms_.size() ; ++i)
    {
        if (geoms_[i] && geoms_[i]->collision_geometry_)
        {
            boost::shared_ptr<fcl::CollisionObject>& collision_object = fcl_obj.collision_objects_[index];
            collision_object->setTransform(transform2fcl(state.getCollisionBodyTransform(geoms_[i]->collision_geometry_data_->ptr.link,
                                           geoms_[i]->collision_geometry_data_->shape_index)));
            collision_object->computeAABB();
            ++index;
        }
    }
    manager_.manager_->update();

    std::vector<const robot_state::AttachedBody*> ab;
    state.getAttachedBodies(ab);
    ROS_ASSERT(ab.size() == 0);
    /*
    for (std::size_t j = 0 ; j < ab.size() ; ++j)
    {
        std::vector<FCLGeometryConstPtr> objs;
        getAttachedBodyObjects(ab[j], objs);
        const EigenSTL::vector_Affine3d &ab_t = ab[j]->getGlobalCollisionBodyTransforms();
        for (std::size_t k = 0 ; k < objs.size() ; ++k)
            if (objs[k]->collision_geometry_)
            {
                fcl::CollisionObject *collObj = new fcl::CollisionObject(objs[k]->collision_geometry_, transform2fcl(ab_t[k]));
                fcl_obj.collision_objects_.push_back(boost::shared_ptr<fcl::CollisionObject>(collObj));
                // we copy the shared ptr to the CollisionGeometryData, as this is not stored by the class itself,
                // and would be destroyed when objs goes out of scope.
                fcl_obj.collision_geometry_.push_back(objs[k]);
            }
    }
    */
}


void CollisionRobotFCLDerivatives::checkSelfCollision(const CollisionRequest &req, CollisionResult &res, const robot_state::RobotState &state) const
{
	checkSelfCollisionDerivativesHelper(req, res, state, NULL);
}

void CollisionRobotFCLDerivatives::checkSelfCollision(const CollisionRequest &req, CollisionResult &res, const robot_state::RobotState &state,
		const AllowedCollisionMatrix &acm) const
{
	checkSelfCollisionDerivativesHelper(req, res, state, &acm);
}

double CollisionRobotFCLDerivatives::distanceSelf(const robot_state::RobotState &state) const
{
	return distanceSelfDerivativesHelper(state, NULL);
}

double CollisionRobotFCLDerivatives::distanceSelf(const robot_state::RobotState &state, const collision_detection::AllowedCollisionMatrix &acm) const
{
	return distanceSelfDerivativesHelper(state, &acm);
}

void CollisionRobotFCLDerivatives::checkSelfCollisionDerivativesHelper(const CollisionRequest &req, CollisionResult &res, const robot_state::RobotState &state,
		const AllowedCollisionMatrix *acm) const
{
	CollisionData cd(&req, &res, acm);
	cd.enableGroup(getRobotModel());

	CollisionDataDerivatives cdd;
	cdd.cd = &cd;

    manager_.manager_->collide(&cdd, &CollisionRobotFCLDerivatives::collisionCallback);
	if (req.distance)
		res.distance = distanceSelfDerivativesHelper(state, acm);
}

double CollisionRobotFCLDerivatives::distanceSelfDerivativesHelper(const robot_state::RobotState &state, const AllowedCollisionMatrix *acm) const
{
	FCLManager manager;
	allocSelfCollisionBroadPhase(state, manager);

	CollisionRequest req;
	CollisionResult res;
	CollisionData cd(&req, &res, acm);
	cd.enableGroup(getRobotModel());

	CollisionDataDerivatives cdd;
	cdd.cd = &cd;

	manager.manager_->distance(&cdd, &CollisionRobotFCLDerivatives::distanceCallback);

	return res.distance;
}

bool CollisionRobotFCLDerivatives::collisionCallback(fcl::CollisionObject *o1, fcl::CollisionObject *o2, void *data)
{
	CollisionDataDerivatives *cdd = reinterpret_cast<CollisionDataDerivatives*>(data);
	CollisionData *cdata = cdd->cd;
	if (cdata->done_)
		return true;
	const CollisionGeometryData *cd1 = static_cast<const CollisionGeometryData*>(o1->getCollisionGeometry()->getUserData());
	const CollisionGeometryData *cd2 = static_cast<const CollisionGeometryData*>(o2->getCollisionGeometry()->getUserData());

	// do not collision check geoms part of the same object / link / attached body
	if (cd1->sameObject(*cd2))
		return false;

	// If active components are specified
	if (cdata->active_components_only_)
	{
		const robot_model::LinkModel *l1 = cd1->type == BodyTypes::ROBOT_LINK ? cd1->ptr.link : (cd1->type == BodyTypes::ROBOT_ATTACHED ? cd1->ptr.ab->getAttachedLink() : NULL);
		const robot_model::LinkModel *l2 = cd2->type == BodyTypes::ROBOT_LINK ? cd2->ptr.link : (cd2->type == BodyTypes::ROBOT_ATTACHED ? cd2->ptr.ab->getAttachedLink() : NULL);

		// If neither of the involved components is active
		if ((!l1 || cdata->active_components_only_->find(l1) == cdata->active_components_only_->end()) &&
				(!l2 || cdata->active_components_only_->find(l2) == cdata->active_components_only_->end()))
			return false;
	}

	// use the collision matrix (if any) to avoid certain collision checks
	DecideContactFn dcf;
	bool always_allow_collision = false;
	if (cdata->acm_)
	{
		AllowedCollision::Type type;
		bool found = cdata->acm_->getAllowedCollision(cd1->getID(), cd2->getID(), type);
		if (found)
		{
			// if we have an entry in the collision matrix, we read it
			if (type == AllowedCollision::ALWAYS)
			{
				always_allow_collision = true;
				if (cdata->req_->verbose)
					logDebug("Collision between '%s' (type '%s') and '%s' (type '%s') is always allowed. No contacts are computed.",
							 cd1->getID().c_str(),
							 cd1->getTypeString().c_str(),
							 cd2->getID().c_str(),
							 cd2->getTypeString().c_str());
			}
			else if (type == AllowedCollision::CONDITIONAL)
			{
				cdata->acm_->getAllowedCollision(cd1->getID(), cd2->getID(), dcf);
				if (cdata->req_->verbose)
					logDebug("Collision between '%s' and '%s' is conditionally allowed", cd1->getID().c_str(), cd2->getID().c_str());
			}
		}
	}

	// check if a link is touching an attached object
	if (cd1->type == BodyTypes::ROBOT_LINK && cd2->type == BodyTypes::ROBOT_ATTACHED)
	{
		const std::set<std::string> &tl = cd2->ptr.ab->getTouchLinks();
		if (tl.find(cd1->getID()) != tl.end())
		{
			always_allow_collision = true;
			if (cdata->req_->verbose)
				logDebug("Robot link '%s' is allowed to touch attached object '%s'. No contacts are computed.",
						 cd1->getID().c_str(), cd2->getID().c_str());
		}
	}
	else if (cd2->type == BodyTypes::ROBOT_LINK && cd1->type == BodyTypes::ROBOT_ATTACHED)
	{
		const std::set<std::string> &tl = cd1->ptr.ab->getTouchLinks();
		if (tl.find(cd2->getID()) != tl.end())
		{
			always_allow_collision = true;
			if (cdata->req_->verbose)
				logDebug("Robot link '%s' is allowed to touch attached object '%s'. No contacts are computed.",
						 cd2->getID().c_str(), cd1->getID().c_str());
		}
	}
	// bodies attached to the same link should not collide
	if (cd1->type == BodyTypes::ROBOT_ATTACHED && cd2->type == BodyTypes::ROBOT_ATTACHED)
	{
		if (cd1->ptr.ab->getAttachedLink() == cd2->ptr.ab->getAttachedLink())
			always_allow_collision = true;
	}

	// if collisions are always allowed, we are done
	if (always_allow_collision)
		return false;

	if (cdata->req_->verbose)
		logDebug("Actually checking collisions between %s and %s", cd1->getID().c_str(), cd2->getID().c_str());

	// see if we need to compute a contact
	std::size_t want_contact_count = 0;
	if (cdata->req_->contacts)
		if (cdata->res_->contact_count < cdata->req_->max_contacts)
		{
			std::size_t have;
			if (cd1->getID() < cd2->getID())
			{
				std::pair<std::string, std::string> cp(cd1->getID(), cd2->getID());
				have = cdata->res_->contacts.find(cp) != cdata->res_->contacts.end() ? cdata->res_->contacts[cp].size() : 0;
			}
			else
			{
				std::pair<std::string, std::string> cp(cd2->getID(), cd1->getID());
				have = cdata->res_->contacts.find(cp) != cdata->res_->contacts.end() ? cdata->res_->contacts[cp].size() : 0;
			}
			if (have < cdata->req_->max_contacts_per_pair)
				want_contact_count = std::min(cdata->req_->max_contacts_per_pair - have, cdata->req_->max_contacts - cdata->res_->contact_count);
		}

	if (dcf)
	{
		// if we have a decider for allowed contacts, we need to look at all the contacts
		bool enable_cost = cdata->req_->cost;
		std::size_t num_max_cost_sources = cdata->req_->max_cost_sources;
		bool enable_contact = true;
		fcl::CollisionResult col_result;
		int num_contacts = fcl::collide(o1, o2, fcl::CollisionRequest(std::numeric_limits<size_t>::max(), enable_contact, num_max_cost_sources, enable_cost), col_result);
		if (num_contacts > 0)
		{
			if (cdata->req_->verbose)
				logInform("Found %d contacts between '%s' and '%s'. These contacts will be evaluated to check if they are accepted or not",
						  num_contacts, cd1->getID().c_str(), cd2->getID().c_str());
			Contact c;
			const std::pair<std::string, std::string> &pc = cd1->getID() < cd2->getID() ?
					std::make_pair(cd1->getID(), cd2->getID()) : std::make_pair(cd2->getID(), cd1->getID());
			for (int i = 0 ; i < num_contacts ; ++i)
			{
				fcl2contact(col_result.getContact(i), c);
				// if the contact is  not allowed, we have a collision
				if (dcf(c) == false)
				{
					// store the contact, if it is needed
					if (want_contact_count > 0)
					{
						--want_contact_count;
						cdata->res_->contacts[pc].push_back(c);
						cdata->res_->contact_count++;
						if (cdata->req_->verbose)
							logInform("Found unacceptable contact between '%s' and '%s'. Contact was stored.",
									  cd1->getID().c_str(), cd2->getID().c_str());
					}
					else if (cdata->req_->verbose)
						logInform("Found unacceptable contact between '%s' (type '%s') and '%s' (type '%s'). Contact was stored.",
								  cd1->getID().c_str(), cd1->getTypeString().c_str(),
								  cd2->getID().c_str(), cd2->getTypeString().c_str());
					cdata->res_->collision = true;
					if (want_contact_count == 0)
						break;
				}
			}
		}

		if (enable_cost)
		{
			std::vector<fcl::CostSource> cost_sources;
			col_result.getCostSources(cost_sources);

			CostSource cs;
			for (std::size_t i = 0; i < cost_sources.size(); ++i)
			{
				fcl2costsource(cost_sources[i], cs);
				cdata->res_->cost_sources.insert(cs);
				while (cdata->res_->cost_sources.size() > cdata->req_->max_cost_sources)
					cdata->res_->cost_sources.erase(--cdata->res_->cost_sources.end());
			}
		}
	}
	else
	{
		if (want_contact_count > 0)
		{
			// otherwise, we need to compute more things
			bool enable_cost = cdata->req_->cost;
			std::size_t num_max_cost_sources = cdata->req_->max_cost_sources;
			bool enable_contact = true;

			fcl::CollisionResult col_result;
			int num_contacts = fcl::collide(o1, o2, fcl::CollisionRequest(want_contact_count, enable_contact, num_max_cost_sources, enable_cost), col_result);
			if (num_contacts > 0)
			{
				int num_contacts_initial = num_contacts;

				// make sure we don't get more contacts than we want
				if (want_contact_count >= (std::size_t)num_contacts)
					want_contact_count -= num_contacts;
				else
				{
					num_contacts = want_contact_count;
					want_contact_count = 0;
				}

				if (cdata->req_->verbose)
					logInform("Found %d contacts between '%s' (type '%s') and '%s' (type '%s'), which constitute a collision. %d contacts will be stored",
							  num_contacts_initial,
							  cd1->getID().c_str(), cd1->getTypeString().c_str(),
							  cd2->getID().c_str(), cd2->getTypeString().c_str(),
							  num_contacts);

				const std::pair<std::string, std::string> &pc = cd1->getID() < cd2->getID() ?
						std::make_pair(cd1->getID(), cd2->getID()) : std::make_pair(cd2->getID(), cd1->getID());
				cdata->res_->collision = true;
				for (int i = 0 ; i < num_contacts ; ++i)
				{
					Contact c;
					fcl2contact(col_result.getContact(i), c);
					cdata->res_->contacts[pc].push_back(c);
					cdata->res_->contact_count++;
				}
			}

			if (enable_cost)
			{
				std::vector<fcl::CostSource> cost_sources;
				col_result.getCostSources(cost_sources);

				CostSource cs;
				for (std::size_t i = 0; i < cost_sources.size(); ++i)
				{
					fcl2costsource(cost_sources[i], cs);
					cdata->res_->cost_sources.insert(cs);
					while (cdata->res_->cost_sources.size() > cdata->req_->max_cost_sources)
						cdata->res_->cost_sources.erase(--cdata->res_->cost_sources.end());
				}
			}
		}
		else
		{
			bool enable_cost = cdata->req_->cost;
			std::size_t num_max_cost_sources = cdata->req_->max_cost_sources;
			bool enable_contact = false;
			fcl::CollisionResult col_result;
			int num_contacts = fcl::collide(o1, o2, fcl::CollisionRequest(1, enable_contact, num_max_cost_sources, enable_cost), col_result);
			if (num_contacts > 0)
			{
				cdata->res_->collision = true;
				if (cdata->req_->verbose)
					logInform("Found a contact between '%s' (type '%s') and '%s' (type '%s'), which constitutes a collision. Contact information is not stored.",
							  cd1->getID().c_str(), cd1->getTypeString().c_str(), cd2->getID().c_str(), cd2->getTypeString().c_str());
			}

			if (enable_cost)
			{
				std::vector<fcl::CostSource> cost_sources;
				col_result.getCostSources(cost_sources);

				CostSource cs;
				for (std::size_t i = 0; i < cost_sources.size(); ++i)
				{
					fcl2costsource(cost_sources[i], cs);
					cdata->res_->cost_sources.insert(cs);
					while (cdata->res_->cost_sources.size() > cdata->req_->max_cost_sources)
						cdata->res_->cost_sources.erase(--cdata->res_->cost_sources.end());
				}
			}
		}
	}


	if (cdata->res_->collision)
		if (!cdata->req_->contacts || cdata->res_->contact_count >= cdata->req_->max_contacts)
		{
			if (!cdata->req_->cost)
				cdata->done_ = true;
			if (cdata->req_->verbose)
				logInform("Collision checking is considered complete (collision was found and %u contacts are stored)",
						  (unsigned int)cdata->res_->contact_count);
		}

	if (!cdata->done_ && cdata->req_->is_done)
	{
		cdata->done_ = cdata->req_->is_done(*cdata->res_);
		if (cdata->done_ && cdata->req_->verbose)
			logInform("Collision checking is considered complete due to external callback. %s was found. %u contacts are stored.",
					  cdata->res_->collision ? "Collision" : "No collision", (unsigned int)cdata->res_->contact_count);
	}

	return cdata->done_;
}
bool CollisionRobotFCLDerivatives::distanceCallback(fcl::CollisionObject* o1, fcl::CollisionObject* o2, void *data, double& min_dist)
{
	CollisionDataDerivatives* cdd = reinterpret_cast<CollisionDataDerivatives*>(data);
	CollisionData* cdata = cdd->cd;

	const CollisionGeometryData* cd1 = static_cast<const CollisionGeometryData*>(o1->getCollisionGeometry()->getUserData());
	const CollisionGeometryData* cd2 = static_cast<const CollisionGeometryData*>(o2->getCollisionGeometry()->getUserData());

	// If active components are specified
	if (cdata->active_components_only_)
	{
		const robot_model::LinkModel *l1 = cd1->type == BodyTypes::ROBOT_LINK ? cd1->ptr.link : (cd1->type == BodyTypes::ROBOT_ATTACHED ? cd1->ptr.ab->getAttachedLink() : NULL);
		const robot_model::LinkModel *l2 = cd2->type == BodyTypes::ROBOT_LINK ? cd2->ptr.link : (cd2->type == BodyTypes::ROBOT_ATTACHED ? cd2->ptr.ab->getAttachedLink() : NULL);

		// If neither of the involved components is active
		if ((!l1 || cdata->active_components_only_->find(l1) == cdata->active_components_only_->end()) &&
				(!l2 || cdata->active_components_only_->find(l2) == cdata->active_components_only_->end()))
		{
			min_dist = cdata->res_->distance;
			return cdata->done_;
		}
	}

	// use the collision matrix (if any) to avoid certain distance checks
	bool always_allow_collision = false;
	if (cdata->acm_)
	{
		AllowedCollision::Type type;

		bool found = cdata->acm_->getAllowedCollision(cd1->getID(), cd2->getID(), type);
		if (found)
		{
			// if we have an entry in the collision matrix, we read it
			if (type == AllowedCollision::ALWAYS)
			{
				always_allow_collision = true;
				if (cdata->req_->verbose)
					logDebug("Collision between '%s' and '%s' is always allowed. No contacts are computed.",
							 cd1->getID().c_str(), cd2->getID().c_str());
			}
		}
	}

	// check if a link is touching an attached object
	if (cd1->type == BodyTypes::ROBOT_LINK && cd2->type == BodyTypes::ROBOT_ATTACHED)
	{
		const std::set<std::string> &tl = cd2->ptr.ab->getTouchLinks();
		if (tl.find(cd1->getID()) != tl.end())
		{
			always_allow_collision = true;
			if (cdata->req_->verbose)
				logDebug("Robot link '%s' is allowed to touch attached object '%s'. No contacts are computed.",
						 cd1->getID().c_str(), cd2->getID().c_str());
		}
	}
	else
	{
		if (cd2->type == BodyTypes::ROBOT_LINK && cd1->type == BodyTypes::ROBOT_ATTACHED)
		{
			const std::set<std::string> &tl = cd1->ptr.ab->getTouchLinks();
			if (tl.find(cd2->getID()) != tl.end())
			{
				always_allow_collision = true;
				if (cdata->req_->verbose)
					logDebug("Robot link '%s' is allowed to touch attached object '%s'. No contacts are computed.",
							 cd2->getID().c_str(), cd1->getID().c_str());
			}
		}
	}

	if(always_allow_collision)
	{
		min_dist = cdata->res_->distance;
		return cdata->done_;
	}

	if (cdata->req_->verbose)
		logDebug("Actually checking collisions between %s and %s", cd1->getID().c_str(), cd2->getID().c_str());

	fcl::DistanceResult dist_result;
	dist_result.update(cdata->res_->distance, NULL, NULL, fcl::DistanceResult::NONE, fcl::DistanceResult::NONE); // can be faster
	double d = fcl::distance(o1, o2, fcl::DistanceRequest(), dist_result);

	if(d < 0)
	{
		cdata->done_ = true;
		cdata->res_->distance = -1;
	}
	else
	{
		if(cdata->res_->distance > d)
			cdata->res_->distance = d;
	}

	min_dist = cdata->res_->distance;

	return cdata->done_;
}

}



