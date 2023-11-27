#include "lyapunovController.h"

/*
INPUTS:
    x_traj_init: starting pose of the trajectory
    Kp : feedback proportional constant for longitudinal velocity
    Ktheta : feedback proportional constant for angular velocity
    dt: time step used for the trajectory generation
*/
LyapController::LyapController(double Kp, double Ktheta, double dt)
{

    this->Kp = Kp;
    this->Ktheta = Ktheta;
    this->current_time = 0.0;
    this->time_end = 0.0;
    this->e_x = 0.0;
    this->e_y = 0.0;
    this->e_theta = 0.0;

    Eigen::VectorXd u_init(2);    // init control input
    Eigen::VectorXd pose_traj_init(3);    // init state for the trajectory
    u_init.setZero();
    pose_traj_init.setZero();
    std::map<std::string, double> params{{"dt", double(dt)}};
    this->RobotModel = std::make_shared<UnicycleModel> (pose_traj_init, u_init, params);
    finished = true;
}


void LyapController::step(const Eigen::Vector3d& pose, Eigen::Vector2d & vel_out)
{
    computeLaw(pose, vel_out);
    endReached();
}

/*
INPUTS
    pose: current robot pose
OUTPUTS
    u_out: control input obtained following the control law
*/
void LyapController::computeLaw(const Eigen::Vector3d& pose, Eigen::Vector2d  & u_out)
{
    if(u_desired.empty() || pose_desired.empty())
    {
        u_out.setZero();
        throw DESIRED_TRAJECTORY_INCOMPLETE;
    }
    else if(finished)
    {
        std::cout << "Trajectory control finished" << std::endl;
        u_out.setZero();
        return;
    }

    Eigen::Vector2d du;
    Eigen::Vector2d u_ref;
    Eigen::Vector3d pose_ref;
    
    getControlInputDesiredOnTime(current_time, &u_ref);
    getPoseDesiredOnTime(current_time, &pose_ref);
    updateTrackingErrors(pose_ref, pose);

    double alpha = pose(2) + pose_ref(2);
    double psi   = atan2(e_y, e_x);
    double v_ref = u_ref(0);
    double e_xy  = sqrt(pow(e_x, 2) + pow(e_y, 2));

    du(0) = -Kp * e_xy * cos(pose(2) - psi);
    du(1) = -Ktheta * e_theta - v_ref * sinc(e_theta * 0.5) * sin(psi - alpha * 0.5);
    (u_out) << u_ref + du;    
}

void LyapController::updateTrackingErrors(const Eigen::Vector3d& pose_ref, const Eigen::Vector3d& pose)
{
    this->e_x   = pose(0) - pose_ref(0);
    this->e_y   = pose(1) - pose_ref(1);
    this->e_theta  = angleWithinPI(pose(2) - pose_ref(2));
}

void LyapController::copyTrajectory(const std::vector<double>& v, const std::vector<double>& omega, const std::vector<double>& x, const std::vector<double>& y, const std::vector<double>& theta)
{
    for(int i = 0; i < int(v.size()); i++)
    {
        addToInputDesired(v.at(i), omega.at(i));        
    }
    for(int i = 0; i < int(x.size()); i++)
    {
        /*
        * Perform a roto-translation of the path
        */
        Eigen::Vector3d pose_with_offset;
        double theta_offset = pose_offset(2);
        double x_with_offset = pose_offset(0) + (cos(theta_offset)*x.at(i) - sin(theta_offset)*y.at(i));
        double y_with_offset = pose_offset(1) + (sin(theta_offset)*x.at(i) + cos(theta_offset)*y.at(i));
        double theta_with_offset = theta_offset + theta.at(i);
        pose_with_offset << x_with_offset, y_with_offset, theta_with_offset;

        // the offset allow to define a trajectory starting from the condition
        // [0,0,0] and adapt it to the global position aquired from systems such
        // as MOCAP
        addStateDesired(pose_with_offset);
    }
    this->time_end = computeMaxTime();
    finished = false;
}

/*
Apply integration of the unicycle mdoel with the desired control input 
*/
void LyapController::generateTrajectory()
{   
    /*trajectory is computed starting from the offset state*/
    RobotModel->resetState(pose_offset);
    for(int i = 0; i < int(u_desired.size()); i++)
    {
        Eigen::Vector3d pose_next;
        integrateState(u_desired.at(i), &pose_next);
        addStateDesired(pose_next);
    }
    this->time_end = computeMaxTime(); // uses the dt and state dimension to calculate tf
    finished = false;
}

void LyapController::integrateState(const Eigen::Vector2d& u, Eigen::Vector3d* pose_next)
{
    Eigen::VectorXd pose_next_dyn(3);
    RobotModel->setControlInput(u);
    RobotModel->integrate();
    RobotModel->getState(&pose_next_dyn);
    *pose_next << pose_next_dyn;
}

void LyapController::addStateDesired(const Eigen::Vector3d& pose_des)
{
    pose_desired.push_back(pose_des);
}


void LyapController::addToInputDesired(double v, double omega)
{
    Eigen::Vector2d u_tmp;
    u_tmp << v, omega;
    u_desired.push_back(u_tmp);
}


/*
Find the desired control input at a given time. The output vector of 
pose is obatined via linear interpolation between values
*/
void LyapController::getControlInputDesiredOnTime(double t, Eigen::Vector2d* u_desired_out) const
{
    Eigen::Vector2d delta_u;

    int index = getIndexIntegerBasedOnTime(t);
    (*u_desired_out) << u_desired.at(index); // takes only a fraction
}

/*
Find the desired pose at a given time. The output vector of 
pose is obatined via linear interpolation between values
*/
void LyapController::getPoseDesiredOnTime(double t, Eigen::Vector3d* pose_desired_out) const
{
    Eigen::Vector3d delta_pose;
    int index = getIndexIntegerBasedOnTime(t);
    (*pose_desired_out) << pose_desired.at(index); // takes only a fraction
}

int LyapController::getIndexIntegerBasedOnTime(double t) const
{
    int n_max = this->pose_desired.size();
    int index = round(t / RobotModel->getStepTime()); // index relative to the starting pose

    if(index >= n_max)
    {
        index = n_max-1;
    }
    return index;
}

double LyapController::getIndexFractionBasedOnTime(double t) const
{
    double intPart, fractPart;
    fractPart = modf(t / RobotModel->getStepTime(), &intPart); 
    return fractPart; 
}

/*
Check if a given pose is under a certain threshold from the last pose of the 
trajectory
*/
bool LyapController::endReached() 
{
    if(this->current_time >= computeMaxTime())
    {
        this->finished = true;
    }
    return this->finished;
}

double LyapController::computeMaxTime() const
{
    return this->RobotModel->getStepTime() * double(this->pose_desired.size()); 
}

std::string LyapController::stringSetupInfo() const
{
    std::ostringstream oss;
    oss << "starting time: " << current_time << " Kp: " << Kp << " K_theta: " << Ktheta << std::endl;
    oss << "x \t| y \t| theta \t| v \t| omega" << std::endl;
	oss << std::setprecision(3);
    int max_idx = pose_desired.size();
    for(int i = 0; i < max_idx && i < MAX_ITER_TO_PRINT; i+= DECIMATION_PRINT)
    {
        oss << (pose_desired.at(i))(0) << "\t | " << (pose_desired.at(i))(1) << "\t | " << (pose_desired.at(i))(2)
            << "\t | " << (u_desired.at(i))(0) << "\t | " << (u_desired.at(i))(1) << std::endl;
    }
    oss << "------------------------------------------------------" << std::endl;
    oss << (pose_desired.at(max_idx-1))(0) << "\t | " << (pose_desired.at(max_idx-1))(1) << "\t | " << (pose_desired.at(max_idx-1))(2)
            << "\t | " << (u_desired.at(max_idx-1))(0) << "\t | " << (u_desired.at(max_idx-1))(1) << std::endl;
    return oss.str();
}