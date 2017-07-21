#include "pch.h"

//////////////////////////////////////////////////////////////////////////
// can't find these headers? Go get cppwinrt now:
// https://github.com/Microsoft/cppwinrt
// These headers wrap all that nasty COM code (flashback aka '94)
// C++ programs can now easily access modern Windows APIs 
// Afterthought: it is highly recommended that all of the documentation is read first
// winrt can be located anywhere but I put in a sub dir called "winrt" in the project includes
#include "winrt/Windows.Foundation.h"
#include "winrt/Windows.Media.Audio.h"
#include "winrt/Windows.Media.MediaProperties.h"


//////////////////////////////////////////////////////////////////////////
// must define this by hand
// must come before using namespace stuff because IUnknown will be ambiguous
MIDL_INTERFACE("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")

IMemoryBufferByteAccess : IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE GetBuffer(BYTE   **value, UINT32 *capacity);
};

//////////////////////////////////////////////////////////////////////////
// notice the namespaces match the header file names
using namespace winrt;

using namespace winrt::Windows::Foundation;

using namespace winrt::Windows::Media;
using namespace winrt::Windows::Media::Devices;
using namespace winrt::Windows::Media::Capture;
using namespace winrt::Windows::Media::Audio;
using namespace winrt::Windows::Media::Render;
using namespace winrt::Windows::Media::MediaProperties;

//////////////////////////////////////////////////////////////////////////
class WindowsAudioGraph
{
public:
	WindowsAudioGraph()
	{

	}

	virtual ~WindowsAudioGraph()
	{

	}

	void Start()
	{
		StartInternal().get();
	}

	void Stop()
	{
		if (m_data_input_node)
		{
			m_data_input_node.Stop();
		}

		if (m_data_output_node)
		{
			m_data_output_node.QuantumStarted(m_quantum_started_token);
			m_data_output_node.Stop();
		}

		if (m_graph)
		{
			m_graph.Stop();
		}
	}

	void RunTest()
	{
		Start();

		Sleep(4000);

		Stop();
	}

private:
	//////////////////////////////////////////////////////////////////////////
	// co_await must have IAsyncAction struct in place
	// IAysncAction is the same as IAsyncOperation except that the operation can return a value
	IAsyncAction StartInternal()
	{
		AudioGraphSettings settings(AudioRenderCategory::Media);
		settings.QuantumSizeSelectionMode(QuantumSizeSelectionMode::LowestLatency);
		CreateAudioGraphResult result = co_await AudioGraph::CreateAsync(settings);

		if (result.Status() == AudioGraphCreationStatus::Success)
		{
			// opened audio graph
			m_graph = result.Graph();

			// Create a device output node
			CreateAudioDeviceOutputNodeResult deviceOutputNodeResult = co_await m_graph.CreateDeviceOutputNodeAsync();
			if (deviceOutputNodeResult.Status() == AudioDeviceNodeCreationStatus::Success)
			{
				m_device_output_node = deviceOutputNodeResult.DeviceOutputNode();

				// Create a device input node using the default audio input device
				CreateAudioDeviceInputNodeResult deviceInputNodeResult = co_await m_graph.CreateDeviceInputNodeAsync(MediaCategory::Other);
				if (deviceInputNodeResult.Status() == AudioDeviceNodeCreationStatus::Success)
				{
					m_device_input_node = deviceInputNodeResult.DeviceInputNode();

					AudioEncodingProperties encoding_properties = m_graph.EncodingProperties();
					//encoding_properties.ChannelCount(m_input_channel_count);
					// yes, the terminology is weird (output node allows use to grab samples from the mic)
					m_data_input_node = m_graph.CreateFrameOutputNode();

					// this is muy importante, it routes audio from the input device to the input data node
					m_device_input_node.AddOutgoingConnection(m_data_input_node);
				}
				else // could not create input node
				{

				}

				// Create the FrameInputNode at the same format as the graph
				AudioEncodingProperties encoding_properties = m_graph.EncodingProperties();
				//nodeEncodingProperties.ChannelCount = 2;
				// yes, the terminology is weird (input node allows use to send samples to the speaker)
				m_data_output_node = m_graph.CreateFrameInputNode(encoding_properties);

				// run this node to the output device
				m_data_output_node.AddOutgoingConnection(m_device_output_node);

				// Initialize the Frame Input Node in the stopped state
				m_data_output_node.Stop();

				// Hook up an event handler so we can start generating samples when needed
				// This event is triggered when the node is required to provide data
				//frame_data_node.QuantumStarted += AudioCallback;
				m_quantum_started_token = m_data_output_node.QuantumStarted([=](auto &&sender, auto &&args)
				{
					AudioOutputCallback(sender, args);
				});

				// Start the graph since we will only start/stop the frame input node
				m_graph.Start();

				//
				if (m_data_output_node)
					m_data_output_node.Start();

				if (m_data_input_node)
				{
					m_data_input_node.Start();
					m_data_input_node.Reset();
				}
			}
			else // could not create output node
			{

			}
		}
		else // could not make AudioGraph
		{

		}
	}

	//////////////////////////////////////////////////////////////////////////
	void AudioOutputCallback(AudioFrameInputNode sender, FrameInputNodeQuantumStartedEventArgs args)
	{
		// Use the FrameInputNodeQuantumStartedEventArgs.RequiredSamples property
		// to determine how many samples are required to fill the quantum with data.
		// Pass an AudioFrame into the AddFrame method to provide the required audio samples.

		// Need to know how many samples are required. In this case, the node is running at the same rate as the rest of the graph
		// For minimum latency, only provide the required amount of samples. Extra samples will introduce additional latency.
		auto output_sample_count = args.RequiredSamples();

		if (0 < output_sample_count)
		{
			// The provided AudioFrame object must have an underlying AudioBuffer with a Length greater than 0 
			// or an E_INVALIDARG error will result.
			// Also, the underlying IMemoryBuffer containing the raw audio data must be block - aligned 
			// based on the bit depth per sample and number of channels.
			// This means that the size of the buffer, in bytes, must be an integral multiple of(BitsPerSample / 8) * ChannelCount of the EncodingProperties property for the node.
			// Attempting to add a frame with a non - aligned audio buffer will result in an E_INVALIDARG error.
			// A maximum of 64 frames can be queued in the AudioFrameInputNode at one time.
			// Attempting to queue more than 64 frames will result in an error.


			AudioFrame input_frame = nullptr;
			AudioBuffer input_buffer = nullptr;
			float *input_data = nullptr;
			int input_channel_count = 0;
			int input_sample_rate = 0;
			int input_sample_count = 0;

			if (m_data_input_node)
			{
				bool consume_input = m_data_input_node.ConsumeInput();

				input_frame = m_data_input_node.GetFrame();
				if (input_frame)
				{
					// process input
					bool input_discontinuity_detected = input_frame.IsDiscontinuous();

					input_channel_count = m_data_input_node.EncodingProperties().ChannelCount();
					input_sample_rate = m_data_input_node.EncodingProperties().SampleRate();

					input_buffer = input_frame.LockBuffer(AudioBufferAccessMode::Read);
					input_data = GetDataPtrFromBuffer(input_buffer);

					// figure out how many samples are in the input buffer
					auto input_bits_per_sample = m_data_input_node.EncodingProperties().BitsPerSample();
					auto input_bytes_per_sample = input_bits_per_sample >> 3;
					auto input_byte_count = input_buffer.Length();
					input_sample_count = input_byte_count / (input_channel_count * input_bytes_per_sample);
				}
			}


			// get all the info concerning the output sample format
			AudioEncodingProperties properties = sender.EncodingProperties();
			auto output_channel_count = properties.ChannelCount();
			auto output_sample_rate = properties.SampleRate();
			auto output_bits_per_sample = properties.BitsPerSample();
			auto output_bytes_per_sample = output_bits_per_sample >> 3;

			unsigned int byte_count = output_sample_count * output_channel_count * output_bytes_per_sample;
			AudioFrame frame(byte_count);

			// grab the samples collected thus for from the microphone / input device
			// get access to the output sample buffer
			{
				AudioBuffer buffer = frame.LockBuffer(AudioBufferAccessMode::Write);
				float* output_data = GetDataPtrFromBuffer(buffer);

				GenerateSineWave(output_data, output_channel_count, output_sample_count, output_sample_rate);

				// route back to speakers
				for (int s = 0; s < output_sample_count && s < input_sample_count; s++)
				{
					output_data[s * output_channel_count] += input_data[s * input_channel_count];
					output_data[s * output_channel_count + 1] += input_data[s * input_channel_count];
				}
			}

			sender.AddFrame(frame);
		}
	}

	//////////////////////////////////////////////////////////////////////////
	float *GetDataPtrFromBuffer(AudioBuffer &buffer)
	{
		IMemoryBufferReference buffer_reference = buffer.CreateReference();

		// this is where I'm stuck, this seems pretty good
		com_ptr<IMemoryBufferByteAccess> byte_buffer_access;

		// this cast is neat, thanks cppwinrt (not a C style cast at all)
		// it actually queries this different interface behind the scenes
		byte_buffer_access = buffer_reference.as<IMemoryBufferByteAccess>();

		// Get the raw buffer from the AudioFrame
		BYTE* data_byte_ptr = nullptr;
		unsigned int byte_read = 0;
		byte_buffer_access->GetBuffer(&data_byte_ptr, &byte_read);

		// Cast to float since the data we are generating is float
		float* data_float_ptr = (float*)data_byte_ptr;

		return data_float_ptr;
	}

	//////////////////////////////////////////////////////////////////////////
	void GenerateSineWave(float *data, int channel_count, int sample_count, double sample_rate)
	{
		// got the raw float pointer 
		// render a sine wav to test for panning and signal integrity
		const float pi = 3.14159265359f;
		float target_frequency = 100.0f;
		float gain = 0.3f;
		// how many samples does it take to make one full sine wave
		float sin_increment = target_frequency * 2.0f*pi / (float)sample_rate;
		for (auto s = 0; s < sample_count; s++)
		{
			float y = sin(m_sin_angle);
			y *= gain;

			// left
			data[s * channel_count] = y;
			// right
			data[s * channel_count + 1] = y;

			m_sin_angle = fmodf(m_sin_angle + sin_increment, 2.0f * pi);
		}
	}


private:
	//
	AudioGraph m_graph = nullptr;

	// this prevents the default constructor from running
	// these guys are nice, they don't allow a default constructor
	AudioDeviceOutputNode m_device_output_node = nullptr;
	AudioDeviceInputNode m_device_input_node = nullptr;

	// the terminology is weird, this is the from the point of view of the speakers / headphones
	// so we feed "in" samples to this node that gets routed to speaker (of some sort)
	AudioFrameInputNode m_data_output_node = nullptr;
	// from the point of view of the microphone, we grab samples "out" of this node
	AudioFrameOutputNode m_data_input_node = nullptr;

	event_token m_quantum_started_token;

	int	m_input_channel_count = 1;


	float m_sin_angle = 0; // for sine wave generation
};

//////////////////////////////////////////////////////////////////////////
int main()
{
	// Initializes the Windows Runtime on the current thread with the specified concurrency model
	init_apartment();

	WindowsAudioGraph audio;
	audio.RunTest();
}




