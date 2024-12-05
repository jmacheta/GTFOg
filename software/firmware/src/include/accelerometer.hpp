#ifndef ACCELEROMETER_HPP_
#define ACCELEROMETER_HPP_


class Accelerometer {
  protected:
    ~Accelerometer() = default;
    friend Accelerometer& accelerometer_instance() noexcept;
};


[[nodiscard]] Accelerometer& accelerometer_instance() noexcept;


#endif