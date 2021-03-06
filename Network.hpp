#ifndef _NETWORK_H_
#define _NETWORK_H_

#include <boost/multi_array.hpp>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <limits>
#include <fstream>
#include <iostream>
#include <numeric>
#include <memory>
#include <random>
#include <vector>
#include "tbb/tbb.h"
#include "Data.hpp"
#include "Params.hpp"

#ifdef NDEBUG
#define UNREACHABLE() __builtin_unreachable()
#else
#define UNREACHABLE() __builtin_trap()
#endif

/// Sigmoid activation function.
struct Sigmoid {
  static float compute(float z) { return 1.0f / (1.0f + std::exp(-z)); }
  static float deriv(float z) { return compute(z) * (1.0f - compute(z)); }
};

/// Rectified linear activation function.
struct ReLU {
  static float compute(float z) { return std::max(0.0f, z); }
  static float deriv(float z) { return z > 0.0f ? 1.0f : 0.0f; }
};

template<float (*activationFnDeriv)(float)>
struct QuadraticCost {
  static float compute(float activation, float label) {
    return 0.5f * std::pow(std::abs(activation - label), 2);
  }
  static float delta(float z, float activation, float label) {
    return (activation - label) * activationFnDeriv(z);
  }
};

struct CrossEntropyCost {
  static float compute(float activation, float label) {
    return (-label * std::log(activation))
             - ((1.0f - label) * std::log(1.0f - activation));
  }
  static float delta(float z, float activation, float label) {
    return activation - label;
  }
};

/// Helper functions for conversions between 1D and 3D coordinates.
static inline unsigned getX(unsigned index, unsigned dimX) {
  return index % dimX;
}
static inline unsigned getY(unsigned index, unsigned dimX, unsigned dimY) {
  return (index / dimX) % dimY;
}
static inline unsigned getZ(unsigned index, unsigned dimX, unsigned dimY) {
  return (index / (dimX * dimY)) /* % dimZ */;
}
static inline unsigned getIndex(unsigned x, unsigned y, unsigned z,
                                unsigned dimX, unsigned dimY) {
  return ((dimX * dimY) * z) + (dimX * y) + x;
}

template<unsigned mbSize>
struct Neuron {
  /// Each neuron in the network can be indexed by a one- or three-dimensional
  /// coordinate, and stores a weighted input, an activation and an error.
  /// x and y are coordinates in the 2D image plane, z indexes depth.
  unsigned index, x, y, z;
  float weightedInputs[mbSize];
  float activations[mbSize];
  float errors[mbSize];
  Neuron(unsigned index) : index(index) {}
  Neuron(unsigned x, unsigned y, unsigned z) : x(x), y(y), z(z) {}
};

template <unsigned mbSize>
struct Layer {
  virtual void initialiseDefaultWeights(std::default_random_engine&) = 0;
  virtual void feedForward(unsigned mb) = 0;
  virtual void calcBwdError(unsigned mb) = 0;
  virtual void backPropogate(unsigned mb) = 0;
  virtual void endBatch(unsigned numTrainingImages) = 0;
  virtual void setInputs(Layer<mbSize> *layer) = 0;
  virtual void setOutputs(Layer<mbSize> *layer) = 0;
  virtual float getBwdError(unsigned index, unsigned mb) = 0;
  virtual float getBwdError(unsigned x, unsigned y, unsigned z, unsigned mb) = 0;
  virtual Neuron<mbSize> &getNeuron(unsigned index) = 0;
  virtual Neuron<mbSize> &getNeuron(unsigned x, unsigned y, unsigned z) = 0;
  virtual unsigned getNumDims() = 0;
  virtual unsigned getDim(unsigned i) = 0;
  virtual unsigned size() = 0;
};

///===--------------------------------------------------------------------===///
/// Input layer.
///===--------------------------------------------------------------------===///
template <unsigned mbSize,
          unsigned imageX,
          unsigned imageY>
class InputLayer : public Layer<mbSize> {
  // x, y, z dimensions of input image.
  boost::multi_array<Neuron<mbSize>*, 3> neurons;

public:
  InputLayer() :
    neurons(boost::extents[imageX][imageY][1]) {
    for (unsigned x = 0; x < imageX; ++x) {
      for (unsigned y = 0; y < imageY; ++y) {
        neurons[x][y][0] = new Neuron<mbSize>(x, y, 0);
      }
    }
  }
  void setImage(Image &image, unsigned mb) {
    assert(image.size() == neurons.num_elements() && "invalid image size");
    for (unsigned i = 0; i < image.size(); ++i) {
      neurons[i % imageX][i / imageX][0]->activations[mb] = image[i];
    }
  }
  void initialiseDefaultWeights(std::default_random_engine&) override {
    UNREACHABLE();
  }
  virtual void calcBwdError(unsigned) override {
    UNREACHABLE();
  }
  void feedForward(unsigned) override {
    UNREACHABLE();
  }
  void backPropogate(unsigned) override {
    UNREACHABLE();
  }
  void endBatch(unsigned) override {
    UNREACHABLE();
  }
  void setInputs(Layer<mbSize>*) override {
    UNREACHABLE();
  }
  void setOutputs(Layer<mbSize>*) override {
    UNREACHABLE();
  }
  float getBwdError(unsigned, unsigned) override {
    UNREACHABLE();
  }
  float getBwdError(unsigned, unsigned, unsigned, unsigned) override {
    UNREACHABLE();
  }
  Neuron<mbSize> &getNeuron(unsigned i) override {
    assert(i < neurons.num_elements() && "Neuron index out of range.");
    return *neurons[i % imageX][i / imageX][0];
  }
  Neuron<mbSize> &getNeuron(unsigned x, unsigned y, unsigned z) override {
    assert(z == 0 && "Input image has depth 1");
    return *neurons[x][y][z];
  }
  unsigned getNumDims() override { return neurons.num_dimensions(); }
  unsigned getDim(unsigned i) override { return neurons.shape()[i]; }
  unsigned size() override { return neurons.num_elements(); }
};

///===--------------------------------------------------------------------===///
/// Fully-connected neuron.
///===--------------------------------------------------------------------===///
template <unsigned mbSize,
          float (*activationFn)(float) = nullptr,
          float (*activationFnDeriv)(float) = nullptr>
class FullyConnectedNeuron : public Neuron<mbSize> {
protected:
  float learningRate;
  float lambda;
  Layer<mbSize> *inputs;
  Layer<mbSize> *outputs;
  std::vector<float> weights;
  float bias;

public:
  FullyConnectedNeuron(unsigned index, float learningRate, float lambda) :
    Neuron<mbSize>(index),
    learningRate(learningRate), lambda(lambda),
    inputs(nullptr), outputs(nullptr) {}

  void initialiseDefaultWeights(std::default_random_engine &gen) {
    // Initialise all weights with random values from normal distribution with
    // mean 0 and stdandard deviation 1, divided by the square root of the
    // number of input connections.
    std::normal_distribution<float> distribution(0, 1.0f);
    for (unsigned i = 0; i < inputs->size(); ++i) {
      float weight = distribution(gen) / std::sqrt(inputs->size());
      weights.push_back(weight);
    }
    bias = distribution(gen);
  }

  void feedForward(unsigned mb) {
    float weightedInput = 0.0f;
    for (unsigned i = 0; i < inputs->size(); ++i) {
      weightedInput += inputs->getNeuron(i).activations[mb] * weights[i];
    }
    weightedInput += bias;
    this->weightedInputs[mb] = weightedInput;
    this->activations[mb] = activationFn(weightedInput);
  }

  void backPropogate(unsigned mb) {
    // Get the weight-error sum component from the next layer, then multiply by
    // the sigmoid derivative to get the error for this neuron.
    float error = outputs->getBwdError(this->index, mb);
    error *= activationFnDeriv(this->weightedInputs[mb]);
    this->errors[mb] = error;
  }

  void endBatch(unsigned numTrainingImages) {
    // For each weight.
    for (unsigned i = 0; i < inputs->size(); ++i) {
      float weightDelta = 0.0f;
      // For each batch element, average input activation x error (rate of
      // change of cost w.r.t. weight) and multiply by learning rate.
      // Note that FC layers can only be followed by FC layers.
      for (unsigned j = 0; j < mbSize; ++j) {
        weightDelta += inputs->getNeuron(i).activations[j] * this->errors[j];
      }
      weightDelta *= learningRate / mbSize;
      float reg = 1.0f - (learningRate * (lambda / numTrainingImages));
      weights[i] *= reg; // Regularisation term.
      weights[i] -= weightDelta;
    }
    // For each batch element, average the errors (error is equal to rate of
    // change of cost w.r.t. bias) and multiply by learning rate.
    float biasDelta = 0.0f;
    for (unsigned j = 0; j < mbSize; ++j) {
      biasDelta += this->errors[j];
    }
    biasDelta *= learningRate / mbSize;
    bias -= biasDelta;
  }

  void setInputs(Layer<mbSize> *inputs) { this->inputs = inputs; }
  void setOutputs(Layer<mbSize> *outputs) { this->outputs = outputs; }
  unsigned numWeights() { return weights.size(); }
  float getWeight(unsigned i) { return weights.at(i); }
};

///===--------------------------------------------------------------------===///
/// Fully-connected layer.
///===--------------------------------------------------------------------===///
template <unsigned mbSize,
          unsigned layerSize,
          unsigned prevSize,
          float (*activationFn)(float),
          float (*activationFnDeriv)(float)>
class FullyConnectedLayer : public Layer<mbSize> {
  using FullyConnectedNeuronTy =
      FullyConnectedNeuron<mbSize, activationFn, activationFnDeriv>;
  Layer<mbSize> *inputs;
  Layer<mbSize> *outputs;
  std::vector<FullyConnectedNeuronTy> neurons;
  boost::multi_array<float, 2> bwdErrors; // [mb][i]

public:
  FullyConnectedLayer(Params params) :
      bwdErrors(boost::extents[mbSize][prevSize]) {
    for (unsigned i = 0; i < layerSize; ++i) {
      auto n = FullyConnectedNeuronTy(i, params.learningRate, params.lambda);
      neurons.push_back(n);
    }
  }

  void setInputs(Layer<mbSize> *layer) override {
    inputs = layer;
    for (auto &neuron : neurons) {
      neuron.setInputs(layer);
    }
  }

  void setOutputs(Layer<mbSize> *layer) override {
    outputs = layer;
    for (auto &neuron : neurons) {
      neuron.setOutputs(layer);
    }
  }

  void initialiseDefaultWeights(std::default_random_engine &gen) override {
    for (auto &neuron : neurons) {
      neuron.initialiseDefaultWeights(gen);
    }
  }

  void feedForward(unsigned mb) override {
    for (auto &neuron : neurons) {
      neuron.feedForward(mb);
    }
  }

  /// Calculate the l+1 component of the error for each neuron in prev layer.
  void calcBwdError(unsigned mb) override {
    for (unsigned i = 0; i < inputs->size(); ++i) {
      float error = 0.0f;
      for (auto &neuron : neurons) {
        error += neuron.getWeight(i) * neuron.errors[mb];
      }
      bwdErrors[mb][i] = error;
    }
  }

  /// Update errors from next layer.
  void backPropogate(unsigned mb) override {
    for (auto &neuron : neurons) {
      neuron.backPropogate(mb);
    }
  }

  void endBatch(unsigned numTrainingImages) override {
    for (auto &neuron : neurons) {
      neuron.endBatch(numTrainingImages);
    }
  }

  float getBwdError(unsigned index, unsigned mb) override {
    return bwdErrors[mb][index];
  }

  float getBwdError(unsigned, unsigned, unsigned, unsigned) override {
    UNREACHABLE();
  }

  Neuron<mbSize> &getNeuron(unsigned index) override {
    return neurons.at(index);
  }

  Neuron<mbSize> &getNeuron(unsigned, unsigned, unsigned) override {
    UNREACHABLE();
  }

  unsigned getNumDims() override { return 1; }

  unsigned getDim(unsigned i) override {
    assert(i == 0 && "Layer is 1D");
    return neurons.size();
  }

  unsigned size() override { return neurons.size(); }
};

///===--------------------------------------------------------------------===///
/// Softmax neuron.
///===--------------------------------------------------------------------===///
template <unsigned mbSize,
          float (*costFn)(float, float),
          float (*costDelta)(float, float, float)>
class SoftMaxNeuron : public FullyConnectedNeuron<mbSize> {

public:
  SoftMaxNeuron(unsigned index, float learningRate, float lambda) :
      FullyConnectedNeuron<mbSize>(index, learningRate, lambda) {}

  void feedForward(unsigned mb) {
    // Only calculate weighted inputs.
    float weightedInput = 0.0f;
    for (unsigned i = 0; i < this->inputs->size(); ++i) {
      weightedInput +=
        this->inputs->getNeuron(i).activations[mb] * this->weights[i];
    }
    weightedInput += this->bias;
    this->weightedInputs[mb] = weightedInput;
  }

  void backPropogate(unsigned) { UNREACHABLE(); }

  void computeOutputError(uint8_t label, unsigned mb) {
    float y = label == this->index ? 1.0f : 0.0f;
    float error = costDelta(this->weightedInputs[mb], this->activations[mb], y);
    this->errors[mb] = error;
  }

  float computeOutputCost(uint8_t label, unsigned mb) {
    return costFn(this->activations[mb], label);
  }

  float sumSquaredWeights() {
    float result = 0.0f;
    for (auto weight : this->weights) {
      result += std::pow(weight, 2.0f);
    }
    return result;
  }
};

///===--------------------------------------------------------------------===///
/// Softmax layer.
///===--------------------------------------------------------------------===///
template <unsigned mbSize,
          unsigned layerSize,
          unsigned prevSize,
          float (*costFn)(float, float),
          float (*costDelta)(float, float, float)>
class SoftMaxLayer : public Layer<mbSize> {
  using SoftMaxNeuronTy = SoftMaxNeuron<mbSize, costFn, costDelta>;
  Layer<mbSize> *inputs;
  Layer<mbSize> *outputs;
  std::vector<SoftMaxNeuronTy> neurons;
  boost::multi_array<float, 2> bwdErrors; // [mb][i]

public:
  SoftMaxLayer(float learningRate, float lambda) :
      bwdErrors(boost::extents[mbSize][prevSize]) {
    for (unsigned i = 0; i < layerSize; ++i) {
      this->neurons.push_back(SoftMaxNeuronTy(i, learningRate, lambda));
    }
  }

  void setInputs(Layer<mbSize> *layer) override {
    inputs = layer;
    for (auto &neuron : neurons) {
      neuron.setInputs(layer);
    }
  }

  void setOutputs(Layer<mbSize>*) override {
    UNREACHABLE();
  }

  void initialiseDefaultWeights(std::default_random_engine &gen) override {
    for (auto &neuron : neurons) {
      neuron.initialiseDefaultWeights(gen);
    }
  }

  void feedForward(unsigned mb) override {
    // Calculate weighted inputs for each neuron.
    // Sum the exponential values of the weighted inputs across neurons.
    float sum = 0.0f;
    for (auto &neuron : neurons) {
      neuron.feedForward(mb);
      sum += std::exp(neuron.weightedInputs[mb]);
    }
    // Calculate each of the neuron's activations.
    for (auto &neuron : neurons) {
      neuron.activations[mb] = std::exp(neuron.weightedInputs[mb]) / sum;
    }
  }

  /// Calculate the l+1 component of the error for each neuron in prev layer.
  void calcBwdError(unsigned mb) override {
    for (unsigned i = 0; i < inputs->size(); ++i) {
      float error = 0.0f;
      for (auto &neuron : neurons) {
        error += neuron.getWeight(i) * neuron.errors[mb];
      }
      bwdErrors[mb][i] = error;
    }
  }

  /// Update errors from next layer.
  void backPropogate(unsigned mb) override {
    for (auto &neuron : neurons) {
      neuron.backPropogate(mb);
    }
  }

  void endBatch(unsigned numTrainingImages) override {
    for (auto &neuron : neurons) {
      neuron.endBatch(numTrainingImages);
    }
  }

  /// Determine the index of the highest output activation.
  unsigned readOutput(unsigned mb) {
    unsigned result = 0;
    float max = std::numeric_limits<float>::min();
    for (unsigned i = 0; i < neurons.size(); ++i) {
      float output = neurons[i].activations[mb];
      if (output > max) {
        result = i;
        max = output;
      }
    }
    return result;
  }

  void computeOutputError(uint8_t label, unsigned mb) {
    for (auto &neuron : neurons) {
      neuron.computeOutputError(label, mb);
    }
  }

  float computeOutputCost(uint8_t label, unsigned mb) {
    float outputCost = 0.0f;
    for (auto &neuron : neurons) {
      neuron.computeOutputCost(label, mb);
    }
    return outputCost;
  }

  float sumSquaredWeights() {
    float result = 0.0f;
    for (auto &neuron : neurons) {
      result += neuron.sumSquaredWeights();
    }
    return result;
  }

  float getBwdError(unsigned index, unsigned mb) override {
    return bwdErrors[mb][index];
  }

  float getBwdError(unsigned, unsigned, unsigned, unsigned) override {
    UNREACHABLE();
  }

  Neuron<mbSize> &getNeuron(unsigned index) override {
    return neurons.at(index);
  }

  Neuron<mbSize> &getNeuron(unsigned, unsigned, unsigned) override {
    UNREACHABLE();
  }

  unsigned getNumDims() override { return 1; }

  unsigned getDim(unsigned i) override {
    assert(i == 0 && "Layer is 1D");
    return neurons.size();
  }

  unsigned size() override { return neurons.size(); }
};

///===--------------------------------------------------------------------===///
/// Convolutional neuron.
///===--------------------------------------------------------------------===///
template <unsigned mbSize,
          float (*activationFn)(float),
          float (*activationFnDeriv)(float)>
class ConvNeuron : public Neuron<mbSize> {
  Layer<mbSize> *inputs;
  Layer<mbSize> *outputs;
  unsigned dimX;
  unsigned dimY;

public:
  ConvNeuron(unsigned x, unsigned y, unsigned z, unsigned dimX, unsigned dimY) :
      Neuron<mbSize>(x, y, z), dimX(dimX), dimY(dimY) {}

  void feedForward(boost::multi_array_ref<float, 4> &weights,
                   boost::multi_array_ref<float, 1> &bias,
                   unsigned mb) {
    // Convolve using each weight.
    // (z is the index of the feature map.)
    float weightedInput = 0.0f;
    for (unsigned a = 0; a < weights.shape()[1]; ++a) {
      for (unsigned b = 0; b < weights.shape()[2]; ++b) {
        for (unsigned c = 0; c < weights.shape()[3]; ++c) {
          unsigned inputX = this->x + a;
          unsigned inputY = this->y + b;
          float input = inputs->getNeuron(inputX, inputY, c).activations[mb];
          weightedInput += input * weights[this->z][a][b][c];
        }
      }
    }
    // Add bias and apply non linerarity.
    weightedInput += bias[this->z];
    this->weightedInputs[mb] = weightedInput;
    this->activations[mb] = activationFn(weightedInput);
  }

  void backPropogate(unsigned mb) {
    // If next layer is 1D, map the x, y, z coordinates onto it.
    unsigned index =
      getIndex(this->x, this->y, this->z, this->dimX, this->dimY);
    float error = outputs->getNumDims() == 1
                    ? outputs->getBwdError(index, mb)
                    : outputs->getBwdError(this->x, this->y, this->z, mb);
    error *= activationFnDeriv(this->weightedInputs[mb]);
    this->errors[mb] = error;
  }

  void setInputs(Layer<mbSize> *inputs) { this->inputs = inputs; }
  void setOutputs(Layer<mbSize> *outputs) { this->outputs = outputs; }
};

///===--------------------------------------------------------------------===///
/// Convolutional layer
///
/// kernelX is num cols
/// kernelY is num rows
/// neuron(x, y) is row y, col x
/// weights(a, b) is row b, col a
///===--------------------------------------------------------------------===///
template <unsigned mbSize,
          unsigned kernelX,
          unsigned kernelY,
          unsigned kernelZ,
          unsigned inputX,
          unsigned inputY,
          unsigned inputZ,
          unsigned numFMs,
          float (*activationFn)(float),
          float (*activationFnDeriv)(float)>
class ConvLayer : public Layer<mbSize> {
  using ConvNeuronTy = ConvNeuron<mbSize, activationFn, activationFnDeriv>;
  float learningRate;
  float lambda;
  Layer<mbSize> *inputs;
  Layer<mbSize> *outputs;
  boost::multi_array<float, 1> bias;            // [fm]
  boost::multi_array<float, 4> weights;         // [fm][x][y][z]
  boost::multi_array<ConvNeuronTy*, 3> neurons; // [fm][x][y]
  boost::multi_array<float, 4> bwdErrors;       // [mb][x][y][z]

public:
  ConvLayer(Params params) :
      learningRate(params.learningRate), lambda(params.lambda),
      inputs(nullptr), outputs(nullptr),
      bias(boost::extents[numFMs]),
      weights(boost::extents[numFMs][kernelX][kernelY][kernelZ]),
      neurons(boost::extents[numFMs][inputX-kernelX+1][inputY-kernelY+1]),
      bwdErrors(boost::extents[mbSize][inputX][inputY][inputZ]) {
    static_assert(inputZ == kernelZ, "Kernel depth should match input depth");
    unsigned dimX = neurons.shape()[1];
    unsigned dimY = neurons.shape()[2];
    for (unsigned fm = 0; fm < numFMs; ++fm) {
      for (unsigned x = 0; x < dimX; ++x) {
        for (unsigned y = 0; y < dimY; ++y) {
          neurons[fm][x][y] = new ConvNeuronTy(x, y, fm, dimX, dimY);
        }
      }
    }
  }

  void initialiseDefaultWeights(std::default_random_engine &gen) override {
    // Initialise weights random distribution of mean 0 and standard deviation
    // 1, then scale it by 1/sqrt(number of inputs).
    std::normal_distribution<float> distribution(0, 1.0f);
    float scale =
        std::sqrt(weights.shape()[1] * weights.shape()[2] * weights.shape()[3]);
    for (unsigned fm = 0; fm < weights.shape()[0]; ++fm) {
      for (unsigned a = 0; a < weights.shape()[1]; ++a) {
        for (unsigned b = 0; b < weights.shape()[2]; ++b) {
          for (unsigned c = 0; c < weights.shape()[3]; ++c) {
            weights[fm][a][b][c] = distribution(gen) / scale;
          }
        }
      }
      bias[fm] = distribution(gen);
    }
  }

  void feedForward(unsigned mb) override {
    for (unsigned fm = 0; fm < neurons.shape()[0]; ++fm) {
      for (unsigned x = 0; x < neurons.shape()[1]; ++x) {
        for (unsigned y = 0; y < neurons.shape()[2]; ++y) {
          neurons[fm][x][y]->feedForward(weights, bias, mb);
        }
      }
    }
  }

  void calcBwdError(unsigned mb) override {
    // Calculate the l+1 component of the error for each neuron in prev layer.
    for (unsigned x = 0; x < inputX; ++x) {
      for (unsigned y = 0; y < inputY; ++y) {
        for (unsigned z = 0; z < inputZ; ++z) {
          // Sum over all feature maps.
          float error = 0.0f;
          for (unsigned fm = 0; fm < numFMs; ++fm) {
            for (unsigned a = 0; a < weights.shape()[1]; ++a) {
              for (unsigned b = 0; b < weights.shape()[2]; ++b) {
                if (a <= x && b <= y &&
                    x - a < neurons.shape()[1] &&
                    y - b < neurons.shape()[2]) {
                  float ne = neurons[fm][x - a][y - b]->errors[mb];
                  error += weights[fm][a][b][z] * ne;
                }
              }
            }
          }
          bwdErrors[mb][x][y][z] = error;
        }
      }
    }
  }

  void backPropogate(unsigned mb) override {
    // Update errors from next layer.
    for (unsigned fm = 0; fm < neurons.shape()[0]; ++fm) {
      for (unsigned x = 0; x < neurons.shape()[1]; ++x) {
        for (unsigned y = 0; y < neurons.shape()[2]; ++y) {
          neurons[fm][x][y]->backPropogate(mb);
        }
      }
    }
  }

  void endBatch(unsigned numTrainingImages) override {
    // For each feature map.
    for (unsigned fm = 0; fm < numFMs; ++fm) {
      // For each weight, calculate the delta and update the weight.
      for (unsigned a = 0; a < weights.shape()[1]; ++a) {
        for (unsigned b = 0; b < weights.shape()[2]; ++b) {
          for (unsigned c = 0; c < weights.shape()[3]; ++c) {
            float weightDelta = 0.0f;
            // For each item of the minibatch.
            for (unsigned mb = 0; mb < mbSize; ++mb) {
              // For each neuron.
              for (unsigned x = 0; x < neurons.shape()[1]; ++x) {
                for (unsigned y = 0; y < neurons.shape()[2]; ++y) {
                  float i = inputs->getNeuron(x + a, y + b, c).activations[mb];
                  weightDelta += i * neurons[fm][x][y]->errors[mb];
                }
              }
            }
            weightDelta *= learningRate / mbSize;
            float reg = 1.0f - (learningRate * (lambda / numTrainingImages));
            weights[fm][a][b][c] *= reg; // Regularisation term.
            weights[fm][a][b][c] -= weightDelta;
          }
        }
      }
      // Calculate bias delta and update it.
      float biasDelta = 0.0f;
      // For each item of the minibatch.
      for (unsigned mb = 0; mb < mbSize; ++mb) {
        // For each neuron.
        for (unsigned x = 0; x < neurons.shape()[1]; ++x) {
          for (unsigned y = 0; y < neurons.shape()[2]; ++y) {
            biasDelta += neurons[fm][x][y]->errors[mb];
          }
        }
      }
      biasDelta *= learningRate / mbSize;
      bias[fm] -= biasDelta;
    }
  }

  float getBwdError(unsigned x, unsigned y, unsigned z, unsigned mb) override {
    return bwdErrors[mb][x][y][z];
  }

  void setInputs(Layer<mbSize> *layer) override {
    assert(layer->size() == inputX * inputY * inputZ &&
           "Invalid input layer size");
    inputs = layer;
    std::for_each(neurons.data(), neurons.data() + neurons.num_elements(),
                  [layer](ConvNeuronTy *n){ n->setInputs(layer); });
  }

  void setOutputs(Layer<mbSize> *layer) override {
    outputs = layer;
    std::for_each(neurons.data(), neurons.data() + neurons.num_elements(),
                  [layer](ConvNeuronTy *n){ n->setOutputs(layer); });
  }

  float getBwdError(unsigned, unsigned) override {
    UNREACHABLE(); // No FC layers preceed conv layers.
  }

  Neuron<mbSize> &getNeuron(unsigned index) override {
    // Map a 1D index onto the 3D neurons (for Conv <- FC connections).
    unsigned dimX = neurons.shape()[1];
    unsigned dimY = neurons.shape()[2];
    unsigned x = getX(index, dimX);
    unsigned y = getY(index, dimX, dimY);
    unsigned z = getZ(index, dimX, dimY);
    return *neurons[z][x][y];
  }

  Neuron<mbSize> &getNeuron(unsigned x, unsigned y, unsigned z) override {
    // Feature maps is inner dimension but corresponds to z.
    return *neurons[z][x][y];
  }

  unsigned getDim(unsigned i) override {
    assert(i <= 2 && "Dimension out of range.");
    // Feature maps is inner dimension but corresponds to z.
    return i == 2 ? neurons.shape()[0] : neurons.shape()[i + 1];
  }

  unsigned getNumDims() override { return neurons.num_dimensions(); }
  unsigned size() override { return neurons.num_elements(); }
};

///===--------------------------------------------------------------------===///
/// Max pool layer
///===--------------------------------------------------------------------===///
template <unsigned mbSize,
          unsigned poolX,
          unsigned poolY,
          unsigned inputX,
          unsigned inputY,
          unsigned inputZ>
class MaxPoolLayer : public Layer<mbSize> {
  Layer<mbSize> *inputs;
  Layer<mbSize> *outputs;
  boost::multi_array<Neuron<mbSize>*, 3> neurons; // [x][y][z]

public:
  MaxPoolLayer() :
      inputs(nullptr), outputs(nullptr),
      neurons(boost::extents[inputX / poolX][inputY / poolY][inputZ]) {
    static_assert(inputX % poolX == 0, "Dimension x mismatch with pooling");
    static_assert(inputY % poolY == 0, "Dimension y mismatch with pooling");
    for (unsigned x = 0; x < neurons.shape()[0]; ++x) {
      for (unsigned y = 0; y < neurons.shape()[1]; ++y) {
        for (unsigned z = 0; z < neurons.shape()[2]; ++z) {
          neurons[x][y][z] = new Neuron<mbSize>(x, y, z);
        }
      }
    }
  }

  void initialiseDefaultWeights(std::default_random_engine&) override {
    /* Skip */
  }

  void feedForward(unsigned mb) override {
    // For each neuron in this layer.
    for (unsigned x = 0; x < neurons.shape()[0]; ++x) {
      for (unsigned y = 0; y < neurons.shape()[1]; ++y) {
        for (unsigned z = 0; z < neurons.shape()[2]; ++z) {
          // Take maximum activation over pool area.
          float weightedInput = std::numeric_limits<float>::min();
          for (unsigned a = 0; a < poolX; ++a) {
            for (unsigned b = 0; b < poolY; ++b) {
              unsigned nX = (x * poolX) + a;
              unsigned nY = (y * poolY) + b;
              float input = inputs->getNeuron(nX, nY, z).activations[mb];
              float max = std::max(weightedInput, input);
              neurons[x][y][z]->activations[mb] = max;
            }
          }
        }
      }
    }
  }

  void calcBwdError(unsigned) override { /* Skip */ }
  void backPropogate(unsigned) override { /* Skip */ }

  float getBwdError(unsigned x, unsigned y, unsigned z, unsigned mb) override {
    // Forward the backwards error component from the next layer.
    unsigned nX = x / poolX;
    unsigned nY = y / poolY;
    unsigned nZ = z;
    unsigned dimX = neurons.shape()[0];
    unsigned dimY = neurons.shape()[1];
    // If next layer is 1D, map the x, y, z coordinates onto it.
    unsigned index = getIndex(nX, nY, nZ, dimX, dimY);
    return outputs->getNumDims() == 1
             ? outputs->getBwdError(index, mb)
             : outputs->getBwdError(nX, nY, nZ, mb);
  }

  void endBatch(unsigned) override { /* Skip */ }

  float getBwdError(unsigned, unsigned) override {
    UNREACHABLE(); // No FC layers preceed max-pooling layers.
  }

  void setInputs(Layer<mbSize> *layer) override {
    assert(layer->size() == poolX * poolY * neurons.num_elements() &&
           "invalid input layer size");
    inputs = layer;
  }

  void setOutputs(Layer<mbSize> *layer) override { outputs = layer; }

  Neuron<mbSize> &getNeuron(unsigned index) override {
    // Map a 1D index onto the 3D neurons (for Conv <- FC connections).
    unsigned dimX = neurons.shape()[0];
    unsigned dimY = neurons.shape()[1];
    unsigned x = getX(index, dimX);
    unsigned y = getY(index, dimX, dimY);
    unsigned z = getZ(index, dimX, dimY);
    return *neurons[x][y][z];
  }

  Neuron<mbSize> &getNeuron(unsigned x, unsigned y, unsigned z) override {
    return *neurons[x][y][z];
  }

  unsigned getNumDims() override { return neurons.num_dimensions(); }
  unsigned getDim(unsigned i) override { return neurons.shape()[i]; }
  unsigned size() override { return neurons.num_elements(); }
};

///===--------------------------------------------------------------------===///
/// The network.
///===--------------------------------------------------------------------===///
template <unsigned mbSize,
          unsigned inputX,
          unsigned inputY,
          unsigned softMaxSize,
          unsigned lastLayerSize,
          float (*costFn)(float, float),
          float (*costDelta)(float, float, float)>
class Network {
  using SoftMaxLayerTy = SoftMaxLayer<mbSize, softMaxSize, lastLayerSize,
                                      costFn, costDelta>;
  using LayerTy = Layer<mbSize>;
  Params params;
  InputLayer<mbSize, inputX, inputY> inputLayer;
  SoftMaxLayerTy *softMaxLayer;
  std::vector<LayerTy*> layers;
  std::default_random_engine generator;

public:
  Network(Params params, std::vector<LayerTy*> layers_) :
      params(params), layers(layers_), generator(params.seed) {
    softMaxLayer = new SoftMaxLayerTy(params.learningRate, params.lambda);
    layers.push_back(softMaxLayer);
    // Set neuron inputs.
    layers[0]->setInputs(&inputLayer);
    layers[0]->initialiseDefaultWeights(generator);
    for (unsigned i = 1; i < layers.size(); ++i) {
      layers[i]->setInputs(layers[i - 1]);
      layers[i]->initialiseDefaultWeights(generator);
    }
    // Set neuron outputs.
    for (unsigned i = 0; i < layers.size() - 1; ++i) {
      layers[i]->setOutputs(layers[i + 1]);
    }
  }

  /// The forward pass.
  void feedForward(unsigned mb) {
    for (auto layer : layers) {
      layer->feedForward(mb);
    }
  }

  /// The backward pass.
  void backPropogate(Image &image, uint8_t label, unsigned mb) {
    // Set input.
    inputLayer.setImage(image, mb);
    // Feed forward.
    feedForward(mb);
    // Compute output error in last layer.
    softMaxLayer->computeOutputError(label, mb);
    softMaxLayer->calcBwdError(mb);
    // Backpropagate the error and calculate component for next layer.
    for (int i = layers.size() - 2; i > 0; --i) {
      layers[i]->backPropogate(mb);
      layers[i]->calcBwdError(mb);
    }
    layers[0]->backPropogate(mb);
  }

  void updateMiniBatch(std::vector<Image>::iterator trainingImagesIt,
                       std::vector<uint8_t>::iterator trainingLabelsIt,
                       unsigned numTrainingImages) {
    // For each training image and label, back propogate.
    // Parallelise over the elements of the minibatch to improve performance.
    tbb::parallel_for(size_t(0), size_t(mbSize), [=](size_t i) {
      backPropogate(*(trainingImagesIt + i), *(trainingLabelsIt + i), i);
    });
    // Gradient descent: for every neuron, compute the new weights and biases.
    for (int i = layers.size() - 1; i >= 0; --i) {
      layers[i]->endBatch(numTrainingImages);
    }
  }

  float imageCost(Image &image, uint8_t label,
                  unsigned numImages, float regularisation, unsigned mb) {
    inputLayer.setImage(image, mb);
    feedForward(mb);
    float cost = softMaxLayer->computeOutputCost(label, mb) / numImages;
    cost += regularisation;
    return cost;
  }

  /// Calculate the total cost for a dataset.
  /// Parallelise over the test images (up to the minibatch size).
  float evaluateTotalCost(std::vector<Image> &testImages,
                          std::vector<uint8_t> &testLabels) {
    float regularisation = 0.5f * (params.lambda / testImages.size())
                            * softMaxLayer->sumSquaredWeights();
    float cost = 0.0f;
    for (unsigned i = 0, end = testImages.size(); i < end; i += mbSize) {
      auto mbStart = std::chrono::high_resolution_clock::now();
      // Parallel reduce over the minibatch.
      cost +=
        tbb::parallel_reduce(
          tbb::blocked_range<size_t>(0, mbSize), 0.0f,
          [&](const tbb::blocked_range<size_t> &r, float total) {
            for (size_t mb = r.begin(); mb < r.end(); ++mb) {
              total += imageCost(*(testImages.begin() + i + mb),
                                 *(testLabels.begin() + i + mb),
                                 testImages.size(), regularisation, mb);
            }
            return total;
          }, std::plus<float>());
      auto mbEnd = std::chrono::high_resolution_clock::now();
      auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(mbEnd-mbStart);
      float imagesPerSec = (float(mbSize) / ms.count()) * 1000.0f;
      std::cout << "\rEvaluate cost " << i << " / " << end
                << " (" << imagesPerSec << " imgs/s)";
    }
    return cost;
  }

  bool testImage(Image &image, uint8_t label, unsigned mb) {
    inputLayer.setImage(image, mb);
    feedForward(mb);
    return softMaxLayer->readOutput(mb) == label;
  }

  /// Evaluate the test set and return the number of correct classifications.
  /// Parallelise over the test images (up to the minibatch size).
  unsigned evaluateAccuracy(std::vector<Image> &testImages,
                            std::vector<uint8_t> &testLabels) {
    unsigned result = 0;
    for (unsigned i = 0, end = testImages.size(); i < end; i += mbSize) {
      auto mbStart = std::chrono::high_resolution_clock::now();
      // Parallel reduce over the minibatch.
      result +=
        tbb::parallel_reduce(
          tbb::blocked_range<size_t>(0, mbSize), 0,
          [&](const tbb::blocked_range<size_t> &r, unsigned total) {
            for (size_t mb = r.begin(); mb < r.end(); ++mb) {
              total += testImage(*(testImages.begin() + i + mb),
                                 *(testLabels.begin() + i + mb), mb);
            }
            return total;
          }, std::plus<unsigned>());
      auto mbEnd = std::chrono::high_resolution_clock::now();
      auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(mbEnd-mbStart);
      float imagesPerSec = (float(mbSize) / ms.count()) * 1000.0f;
      std::cout << "\rEvaluate accuracy " << i << " / " << end
                << " (" << imagesPerSec << " imgs/s)";
    }
    return result;
  }

  void SGD(Data &data) {
    // For each epoch.
    for (unsigned epoch = 0; epoch < params.numEpochs; ++epoch) {
      auto epochStart = std::chrono::high_resolution_clock::now();
      // Identically randomly shuffle the training images and labels.
      std::uniform_int_distribution<unsigned> distribution;
      unsigned seed = distribution.operator ()(generator);
      std::shuffle(data.getTrainingLabels().begin(),
                   data.getTrainingLabels().end(),
                   std::default_random_engine(seed));
      std::shuffle(data.getTrainingImages().begin(),
                   data.getTrainingImages().end(),
                   std::default_random_engine(seed));
      // For each mini batch.
      unsigned numTrainingImages = data.getTrainingImages().size();
      for (unsigned i = 0; i < numTrainingImages; i += mbSize) {
        auto mbStart = std::chrono::high_resolution_clock::now();
        updateMiniBatch(data.getTrainingImages().begin() + i,
                        data.getTrainingLabels().begin() + i,
                        mbSize);
        auto mbEnd = std::chrono::high_resolution_clock::now();
        auto ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(mbEnd-mbStart);
        float imagesPerSec = (float(mbSize) / ms.count()) * 1000.0f;
        std::cout << "\rMinibatch " << i << " / " << numTrainingImages
                  << " (" << imagesPerSec << " imgs/s)";
        if (i % params.monitorInterval == 0) {
          std::cout << '\r' << std::string(100, ' ');
          // Evaluate the test set.
          if (params.monitorEvaluationAccuracy) {
            unsigned result = evaluateAccuracy(data.getValidationImages(),
                                               data.getValidationLabels());
            std::cout << '\r' << std::string(100, ' ');
            std::cout << "\rAccuracy on evaluation data: "
                      << result << " / " << data.getValidationImages().size()
                      << '\n';
          }
          if (params.monitorEvaluationCost) {
            float cost = evaluateTotalCost(data.getValidationImages(),
                                           data.getValidationLabels());
            std::cout << '\r' << std::string(100, ' ');
            std::cout << "\rCost on evaluation data: " << cost << "\n";
          }
          if (params.monitorTrainingAccuracy) {
            unsigned result = evaluateAccuracy(data.getTestImages(),
                                               data.getTestLabels());
            std::cout << '\r' << std::string(100, ' ');
            std::cout << "\rAccuracy on test data: "
                      << result << " / " << data.getTestImages().size()
                      << '\n';
          }
          if (params.monitorTrainingCost) {
            float cost = evaluateTotalCost(data.getTestImages(),
                                           data.getTestLabels());
            std::cout << '\r' << std::string(100, ' ');
            std::cout << "\rCost on test data: " << cost << "\n";
          }
        }
      }
      std::cout << '\n';
      // Display end of epoch and time.
      auto epochEnd = std::chrono::high_resolution_clock::now();
      auto s =
        std::chrono::duration_cast<std::chrono::seconds>(epochEnd-epochStart);
      std::cout << "Epoch " << epoch << " complete in " << s.count() << " s.\n";
    }
  }
};

#endif
