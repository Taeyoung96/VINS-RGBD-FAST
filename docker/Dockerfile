FROM osrf/ros:melodic-desktop-full-bionic

# apt-get update
RUN apt-get update

# install essentials
RUN apt install -y gcc
RUN apt install -y g++
RUN apt-get install -y cmake
RUN apt-get install -y wget
RUN apt install -y git

# install ceres
WORKDIR /root
RUN apt-get install -y libgoogle-glog-dev libgflags-dev
RUN apt-get install -y libatlas-base-dev
RUN apt-get install -y libeigen3-dev
RUN apt-get install -y libsuitesparse-dev
RUN wget http://ceres-solver.org/ceres-solver-2.1.0.tar.gz
RUN tar zxf ceres-solver-2.1.0.tar.gz
WORKDIR /root/ceres-solver-2.1.0/build
RUN cmake ..
RUN make
RUN make install

# install sophus
WORKDIR /root
RUN git clone https://github.com/demul/Sophus.git
WORKDIR /root/Sophus
RUN git checkout fix/unit_complex_eror
RUN mkdir build
WORKDIR /root/Sophus/build
RUN cmake ..
RUN make
RUN make install

# install ros dependencies
WORKDIR /root
RUN mkdir ros_ws
WORKDIR /root/ros_ws
RUN apt-get -y install ros-melodic-cv-bridge
RUN apt-get -y install ros-melodic-nodelet
RUN apt-get -y install ros-melodic-tf
RUN apt-get -y install ros-melodic-image-transport
RUN apt-get -y install ros-melodic-rviz

# workspace setting
RUN mkdir src
WORKDIR /root/ros_ws/src

RUN /bin/bash -c '. /opt/ros/melodic/setup.bash; catkin_init_workspace; cd .. && catkin_make'

WORKDIR /root/ros_ws

# Load ROS environment at each run
COPY ./ros_entrypoint.sh /
RUN chmod 755 /ros_entrypoint.sh
ENTRYPOINT ["/ros_entrypoint.sh"]

CMD ["bash"]