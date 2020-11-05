/*
    _____ _____ _____ __
   |   __|     |  |  |  |      The SOUL language
   |__   |  |  |  |  |  |__    Copyright (c) 2019 - ROLI Ltd.
   |_____|_____|_____|_____|

   The code in this file is provided under the terms of the ISC license:

   Permission to use, copy, modify, and/or distribute this software for any purpose
   with or without fee is hereby granted, provided that the above copyright notice and
   this permission notice appear in all copies.

   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD
   TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN
   NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
   DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
   IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
   CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

namespace soul::audioplayer
{

//==============================================================================
class AudioPlayerVenue   : public soul::Venue,
                           private AudioMIDISystem::Callback
{
public:
    AudioPlayerVenue (Requirements r, std::unique_ptr<PerformerFactory> factory)
        : audioSystem (std::move (r)),
          performerFactory (std::move (factory))
    {
        createDeviceEndpoints (audioSystem.getNumInputChannels(),
                               audioSystem.getNumOutputChannels());
    }

    ~AudioPlayerVenue() override
    {
        SOUL_ASSERT (activeSessions.empty());
        audioSystem.setCallback (nullptr);
        performerFactory.reset();
    }

    std::unique_ptr<Venue::Session> createSession() override
    {
        return std::make_unique<AudioPlayerSession> (*this);
    }

    std::vector<EndpointDetails> getSourceEndpoints() override    { return convertEndpointList (sourceEndpoints); }
    std::vector<EndpointDetails> getSinkEndpoints() override      { return convertEndpointList (sinkEndpoints); }

    //==============================================================================
    struct EndpointInfo
    {
        EndpointDetails details;
        int audioChannelIndex = 0;
        bool isMIDI = false;
    };

    struct RenderContext
    {
        uint64_t totalFramesRendered = 0;
        choc::buffer::ChannelArrayView<const float> inputChannels;
        choc::buffer::ChannelArrayView<float> outputChannels;
        const MIDIEvent* midiIn;
        MIDIEvent* midiOut;
        uint32_t frameOffset = 0, midiInCount = 0, midiOutCount = 0, midiOutCapacity = 0;

        template <typename RenderBlockFn>
        void iterateInBlocks (uint32_t maxFramesPerBlock, RenderBlockFn&& render)
        {
            auto framesRemaining = inputChannels.getNumFrames();
            auto context = *this;

            while (framesRemaining != 0)
            {
                auto framesToDo = std::min (maxFramesPerBlock, framesRemaining);
                context.midiIn = midiIn;
                context.midiInCount = 0;

                while (midiInCount != 0)
                {
                    auto time = midiIn->frameIndex;

                    if (time > frameOffset)
                    {
                        framesToDo = std::min (framesToDo, time - frameOffset);
                        break;
                    }

                    ++midiIn;
                    --midiInCount;
                    context.midiInCount++;
                }

                context.inputChannels  = inputChannels.getFrameRange ({ frameOffset, frameOffset + framesToDo });
                context.outputChannels = outputChannels.getFrameRange ({ frameOffset, frameOffset + framesToDo });

                render (context);

                frameOffset += framesToDo;
                framesRemaining -= framesToDo;
                context.totalFramesRendered += framesToDo;
                context.frameOffset += framesToDo;
            }

            midiOutCount = context.midiOutCount;
        }
    };

    //==============================================================================
    struct AudioPlayerSession   : public Venue::Session
    {
        AudioPlayerSession (AudioPlayerVenue& v)  : venue (v)
        {
            performer = venue.performerFactory->createPerformer();
        }

        ~AudioPlayerSession() override
        {
            unload();
        }

        soul::ArrayView<const soul::EndpointDetails> getInputEndpoints() override             { return performer->getInputEndpoints(); }
        soul::ArrayView<const soul::EndpointDetails> getOutputEndpoints() override            { return performer->getOutputEndpoints(); }

        bool load (CompileMessageList& messageList, const Program& program) override
        {
            if (program.isEmpty())
                return false;

            unload();

            if (performer->load (messageList, program))
            {
                setState (SessionState::loaded);
                return true;
            }

            return false;
        }

        void setEndpointActive (const EndpointID& endpointID) override      { performer->getEndpointHandle (endpointID); }

        void setNextInputStreamFrames (EndpointHandle handle, const choc::value::ValueView& frameArray) override
        {
            performer->setNextInputStreamFrames (handle, frameArray);
        }

        void setSparseInputStreamTarget (EndpointHandle handle, const choc::value::ValueView& targetFrameValue, uint32_t numFramesToReachValue) override
        {
            performer->setSparseInputStreamTarget (handle, targetFrameValue, numFramesToReachValue);
        }

        void setInputValue (EndpointHandle handle, const choc::value::ValueView& newValue) override
        {
            performer->setInputValue (handle, newValue);
        }

        void addInputEvent (EndpointHandle handle, const choc::value::ValueView& eventData) override
        {
            performer->addInputEvent (handle, eventData);
        }

        choc::value::ValueView getOutputStreamFrames (EndpointHandle handle) override
        {
            return performer->getOutputStreamFrames (handle);
        }

        void iterateOutputEvents (EndpointHandle handle, Performer::HandleNextOutputEventFn fn) override
        {
            performer->iterateOutputEvents (handle, std::move (fn));
        }

        bool isEndpointActive (const EndpointID& e) override
        {
            return performer->isEndpointActive (e);
        }

        bool link (CompileMessageList& messageList, const BuildSettings& settings) override
        {
            maxBlockSize = settings.maxBlockSize;
            buildOperationList();

            if (state == SessionState::loaded && performer->link (messageList, settings, {}))
            {
                setState (SessionState::linked);
                return true;
            }

            return false;
        }

        bool isRunning() override
        {
            return state == SessionState::running;
        }

        bool start() override
        {
            if (state == SessionState::linked)
            {
                SOUL_ASSERT (performer->isLinked());

                if (venue.startSession (this))
                    setState (SessionState::running);
            }

            return isRunning();
        }

        void stop() override
        {
            if (isRunning())
            {
                venue.stopSession (this);
                setState (SessionState::linked);
                totalFramesRendered = 0;
            }
        }

        void unload() override
        {
            stop();
            performer->unload();
            preRenderOperations.clear();
            postRenderOperations.clear();
            inputCallbacks.clear();
            outputCallbacks.clear();
            connections.clear();
            setState (SessionState::empty);
        }

        Status getStatus() override
        {
            Status s;
            s.state = state;
            s.cpu = venue.audioSystem.getCPULoad();
            s.sampleRate = venue.audioSystem.getSampleRate();
            s.blockSize = venue.audioSystem.getMaxBlockSize();
            s.xruns = performer->getXRuns();

            auto deviceXruns = venue.audioSystem.getXRunCount();

            if (deviceXruns > 0) // < 0 means not known
                s.xruns += (uint32_t) deviceXruns;

            return s;
        }

        void setState (SessionState newState)
        {
            if (state != newState)
            {
                state = newState;

                if (stateChangeCallback != nullptr)
                    stateChangeCallback (state);
            }
        }

        void setStateChangeCallback (StateChangeCallbackFn f) override     { stateChangeCallback = std::move (f); }

        uint64_t getTotalFramesRendered() const override                   { return totalFramesRendered; }

        bool connectSessionInputEndpoint (EndpointID inputID, EndpointID venueSourceID) override
        {
            if (auto venueEndpoint = venue.findEndpoint (venue.sourceEndpoints, venueSourceID))
                return connectInputEndpoint (*venueEndpoint, inputID);

            return false;
        }

        bool connectSessionOutputEndpoint (EndpointID outputID, EndpointID venueSinkID) override
        {
            if (auto venueEndpoint = venue.findEndpoint (venue.sinkEndpoints, venueSinkID))
                return connectOutputEndpoint (*venueEndpoint, outputID);

            return false;
        }

        bool setInputEndpointServiceCallback (EndpointID endpoint, EndpointServiceFn callback) override
        {
            if (! containsEndpoint (performer->getInputEndpoints(), endpoint))
                return false;

            inputCallbacks.push_back ({ performer->getEndpointHandle (endpoint), std::move (callback) });
            return true;
        }

        bool setOutputEndpointServiceCallback (EndpointID endpoint, EndpointServiceFn callback) override
        {
            if (! containsEndpoint (performer->getOutputEndpoints(), endpoint))
                return false;

            outputCallbacks.push_back ({ performer->getEndpointHandle (endpoint), std::move (callback) });
            return true;
        }

        void deviceStopped()
        {
        }

        bool connectInputEndpoint (const EndpointInfo& externalEndpoint, EndpointID inputID)
        {
            for (auto& details : performer->getInputEndpoints())
            {
                if (details.endpointID == inputID)
                {
                    if (isStream (details) && ! externalEndpoint.isMIDI)
                    {
                        connections.push_back ({ externalEndpoint.audioChannelIndex, -1, false, details.endpointID });
                        return true;
                    }

                    if (isEvent (details) && externalEndpoint.isMIDI)
                    {
                        connections.push_back ({ -1, -1, true, details.endpointID });
                        return true;
                    }
                }
            }

            return false;
        }

        bool connectOutputEndpoint (const EndpointInfo& externalEndpoint, EndpointID outputID)
        {
            for (auto& details : performer->getOutputEndpoints())
            {
                if (details.endpointID == outputID)
                {
                    if (isStream (details) && ! externalEndpoint.isMIDI)
                    {
                        connections.push_back ({ -1, externalEndpoint.audioChannelIndex, false, details.endpointID });
                        return true;
                    }
                }
            }

            return false;
        }

        void buildOperationList()
        {
            preRenderOperations.clear();
            postRenderOperations.clear();

            for (auto& connection : connections)
            {
                auto& perf = *performer;
                auto endpointHandle = performer->getEndpointHandle (connection.endpointID);

                if (connection.isMIDI)
                {
                    if (isMIDIEventEndpoint (findDetailsForID (perf.getInputEndpoints(), connection.endpointID)))
                    {
                        auto midiEvent = choc::value::createObject ("soul::midi::Message",
                                                                    "midiBytes", int32_t {});

                        preRenderOperations.push_back ([&perf, endpointHandle, midiEvent] (RenderContext& rc) mutable
                        {
                            for (uint32_t i = 0; i < rc.midiInCount; ++i)
                            {
                                midiEvent.getObjectMemberAt (0).value.set (rc.midiIn[i].getPackedMIDIData());
                                perf.addInputEvent (endpointHandle, midiEvent);
                            }
                        });
                    }
                }
                else if (connection.audioInputStreamIndex >= 0)
                {
                    auto& details = findDetailsForID (perf.getInputEndpoints(), connection.endpointID);
                    auto& frameType = details.getFrameType();
                    auto startChannel = static_cast<uint32_t> (connection.audioInputStreamIndex);
                    auto numChans = frameType.getNumElements();

                    if (frameType.isFloat() || (frameType.isVector() && frameType.getElementType().isFloat()))
                    {
                        choc::buffer::InterleavedBuffer<float> interleaved (numChans, maxBlockSize);

                        preRenderOperations.push_back ([&perf, endpointHandle, startChannel, numChans, interleaved] (RenderContext& rc)
                        {
                            copy (interleaved, rc.inputChannels.getChannelRange ({ startChannel, startChannel + numChans }));

                            perf.setNextInputStreamFrames (endpointHandle, choc::value::create2DArrayView (interleaved.getView().data.data,
                                                                                                           interleaved.getNumFrames(),
                                                                                                           interleaved.getNumChannels()));
                        });
                    }
                    else
                    {
                        SOUL_ASSERT_FALSE;
                    }
                }
                else if (connection.audioOutputStreamIndex >= 0)
                {
                    auto& details = findDetailsForID (perf.getOutputEndpoints(), connection.endpointID);
                    auto& frameType = details.getFrameType();
                    auto startChannel = static_cast<uint32_t> (connection.audioOutputStreamIndex);
                    auto numChans = frameType.getNumElements();

                    if (frameType.isFloat() || (frameType.isVector() && frameType.getElementType().isFloat()))
                    {
                        postRenderOperations.push_back ([&perf, endpointHandle, startChannel, numChans] (RenderContext& rc)
                        {
                            copyIntersectionAndClearOutside (rc.outputChannels.getChannelRange ({ startChannel, startChannel + numChans }),
                                                             getChannelSetFromArray (perf.getOutputStreamFrames (endpointHandle)));
                        });
                    }
                    else
                    {
                        SOUL_ASSERT_FALSE;
                    }
                }
            }
        }

        void processBlock (RenderContext context)
        {
            SOUL_ASSERT (maxBlockSize > 0);
            auto maxFramesPerBlock = std::min (512u, maxBlockSize);
            context.totalFramesRendered = totalFramesRendered;

            context.iterateInBlocks (maxFramesPerBlock, [&] (RenderContext& rc)
            {
                performer->prepare (rc.inputChannels.getNumFrames());

                for (auto& op : preRenderOperations)
                    op (rc);

                for (auto& c : inputCallbacks)
                    c.callback (*this, c.endpointHandle);

                performer->advance();

                for (auto& op : postRenderOperations)
                    op (rc);

                for (auto& c : outputCallbacks)
                    c.callback (*this, c.endpointHandle);
            });

            totalFramesRendered += context.outputChannels.getNumFrames();
        }

        AudioPlayerVenue& venue;
        std::unique_ptr<Performer> performer;
        uint32_t maxBlockSize = 0;
        std::atomic<uint64_t> totalFramesRendered { 0 };
        StateChangeCallbackFn stateChangeCallback;

        struct EndpointCallback
        {
            EndpointHandle endpointHandle;
            EndpointServiceFn callback;
        };

        std::vector<EndpointCallback> inputCallbacks, outputCallbacks;

        struct Connection
        {
            int audioInputStreamIndex = -1, audioOutputStreamIndex = -1;
            bool isMIDI = false;
            EndpointID endpointID;
        };

        std::vector<Connection> connections;
        std::vector<std::function<void(RenderContext&)>> preRenderOperations;
        std::vector<std::function<void(RenderContext&)>> postRenderOperations;

        SessionState state = SessionState::empty;
    };

    //==============================================================================
    bool startSession (AudioPlayerSession* s)
    {
        std::lock_guard<decltype(activeSessionLock)> lock (activeSessionLock);

        if (! contains (activeSessions, s))
            activeSessions.push_back (s);

        audioSystem.setCallback (this);
        return true;
    }

    bool stopSession (AudioPlayerSession* s)
    {
        std::lock_guard<decltype(activeSessionLock)> lock (activeSessionLock);
        removeFirst (activeSessions, [=] (AudioPlayerSession* i) { return i == s; });

        if (activeSessions.empty())
            audioSystem.setCallback (nullptr);

        return true;
    }


private:
    //==============================================================================
    AudioMIDISystem audioSystem;
    std::unique_ptr<PerformerFactory> performerFactory;

    std::vector<EndpointInfo> sourceEndpoints, sinkEndpoints;

    std::recursive_mutex activeSessionLock;
    std::vector<AudioPlayerSession*> activeSessions;

    //==============================================================================
    void createDeviceEndpoints (int numInputChannels, int numOutputChannels)
    {
        if (numInputChannels > 0)
            addEndpoint (sourceEndpoints,
                         EndpointType::stream,
                         EndpointID::create ("defaultIn"),
                         "defaultIn",
                         getVectorType (numInputChannels),
                         0, false);

        if (numOutputChannels > 0)
            addEndpoint (sinkEndpoints,
                         EndpointType::stream,
                         EndpointID::create ("defaultOut"),
                         "defaultOut",
                         getVectorType (numOutputChannels),
                         0, false);

        auto midiMessageType = soul::createMIDIEventEndpointType();

        addEndpoint (sourceEndpoints,
                     EndpointType::event,
                     EndpointID::create ("defaultMidiIn"),
                     "defaultMidiIn",
                     midiMessageType,
                     0, true);

        addEndpoint (sinkEndpoints,
                     EndpointType::event,
                     EndpointID::create ("defaultMidiOut"),
                     "defaultMidiOut",
                     midiMessageType,
                     0, true);
    }

    const EndpointInfo* findEndpoint (ArrayView<EndpointInfo> endpoints, const EndpointID& endpointID) const
    {
        for (auto& e : endpoints)
            if (e.details.endpointID == endpointID)
                return std::addressof (e);

        return {};
    }

    static std::vector<EndpointDetails> convertEndpointList (ArrayView<EndpointInfo> sourceList)
    {
        std::vector<EndpointDetails> result;

        for (auto& e : sourceList)
            result.push_back (e.details);

        return result;
    }

    void renderStarting (double, uint32_t) override {}
    void renderStopped() override {}

    void render (choc::buffer::ChannelArrayView<const float> input,
                 choc::buffer::ChannelArrayView<float> output,
                 const MIDIEvent* midiIn,
                 uint32_t midiInCount) override
    {
        std::lock_guard<decltype(activeSessionLock)> lock (activeSessionLock);

        auto context = RenderContext { 0, input, output, midiIn, nullptr, 0, midiInCount, 0, 0 };

        for (auto& s : activeSessions)
            s->processBlock (context);
    }

    static soul::Type getVectorType (int size)    { return (soul::Type::createVector (soul::PrimitiveType::float32, static_cast<size_t> (size))); }

    static void addEndpoint (std::vector<EndpointInfo>& list, EndpointType endpointType,
                             EndpointID id, std::string name, Type dataType,
                             int audioChannelIndex, bool isMIDI)
    {
        EndpointInfo e;

        e.details.endpointID    = std::move (id);
        e.details.name          = std::move (name);
        e.details.endpointType  = endpointType;
        e.details.dataTypes.push_back (dataType.getExternalType());

        e.audioChannelIndex     = audioChannelIndex;
        e.isMIDI                = isMIDI;

        list.push_back (e);
    }
};

//==============================================================================
std::unique_ptr<Venue> createAudioPlayerVenue (const Requirements& requirements,
                                               std::unique_ptr<PerformerFactory> performerFactory)
{
    return std::make_unique<AudioPlayerVenue> (requirements, std::move (performerFactory));
}

}
