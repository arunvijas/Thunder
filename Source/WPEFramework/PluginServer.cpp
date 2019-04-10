#include "PluginServer.h"
#include "Controller.h"

#ifndef __WIN32__
#include <syslog.h>
#endif

namespace WPEFramework {

ENUM_CONVERSION_BEGIN(Core::ProcessInfo::scheduler)

    { Core::ProcessInfo::BATCH, _TXT("Batch") },
    { Core::ProcessInfo::IDLE, _TXT("Idle") },
    { Core::ProcessInfo::FIFO, _TXT("FIFO") },
    { Core::ProcessInfo::ROUNDROBIN, _TXT("RoundRobin") },
    { Core::ProcessInfo::OTHER, _TXT("Other") },

ENUM_CONVERSION_END(Core::ProcessInfo::scheduler)

ENUM_CONVERSION_BEGIN(PluginHost::InputHandler::type)

    { PluginHost::InputHandler::DEVICE, _TXT("device") },
    { PluginHost::InputHandler::VIRTUAL, _TXT("virtual") },

ENUM_CONVERSION_END(PluginHost::InputHandler::type)

namespace PluginHost
{
    /* static */ Core::ProxyType<Web::Response> Server::Channel::_missingCallsign(Core::ProxyType<Web::Response>::Create());
    /* static */ Core::ProxyType<Web::Response> Server::Channel::_incorrectVersion(Core::ProxyType<Web::Response>::Create());
    /* static */ Core::ProxyType<Web::Response> Server::Channel::WebRequestJob::_missingResponse(Core::ProxyType<Web::Response>::Create());
    /* static */ Core::ProxyType<Web::Response> Server::Service::_missingHandler(Core::ProxyType<Web::Response>::Create());
    /* static */ Core::ProxyType<Web::Response> Server::Service::_unavailableHandler(Core::ProxyType<Web::Response>::Create());

    /* static */ Core::ProxyPoolType<Server::Channel::WebRequestJob> Server::Channel::_webJobs(2);
    /* static */ Core::ProxyPoolType<Server::Channel::JSONElementJob> Server::Channel::_jsonJobs(2);
    /* static */ Core::ProxyPoolType<Server::Channel::TextJob> Server::Channel::_textJobs(2);

#ifdef __WIN32__
    /* static */ const TCHAR* Server::ConfigFile = _T("C:\\Projects\\PluginHost.json");
#else
    /* static */ const TCHAR* Server::ConfigFile = _T("/etc/" EXPAND_AND_QUOTE(NAMESPACE) "/config.json");
#endif

    /* static */ const TCHAR* Server::PluginOverrideFile = _T("PluginHost/override.json");
    /* static */ const TCHAR* Server::PluginConfigDirectory = _T("plugins");
    /* static */ const TCHAR* Server::CommunicatorConnector = _T("COMMUNICATOR_CONNECTOR");

    static const TCHAR _defaultControllerCallsign[] = _T("Controller");
    static const TCHAR _defaultDispatcherCallsign[] = _T("Dispatcher");

    class DefaultSecurity : public ISecurity {
    public:
        DefaultSecurity() = delete;
        DefaultSecurity(const DefaultSecurity&) = delete;
        DefaultSecurity& operator=(const DefaultSecurity&) = delete;

        DefaultSecurity(const string& prefix, const string jsonrpcPath, const string& controllerName)
            : _hasSecurity(true)
            , _controllerPath(prefix + '/' + controllerName)
            , _jsonrpcPath(jsonrpcPath)
            , _controllerName(controllerName)
        {
        }
        ~DefaultSecurity()
        {
        }

    public:
		inline void Security(const bool enabled) {
            _hasSecurity = enabled;
		}
        // Allow a request to be checked before it is offered for processing.
        virtual bool Allowed(const Web::Request& request) const override
        {
            bool result = (_hasSecurity == false);

            if (result == false) {
                // If there is security, maybe this is a valid reuest, althoug this
                // validation is not done by an instance issued by the SecurityOfficer.
                result = ((request.Verb == Web::Request::HTTP_GET) && (request.Path.substr(0, _controllerPath.length()) == _controllerPath));

                if ((result == false) && (request.Verb == Web::Request::HTTP_POST) && (request.HasBody() == true) && (request.Path == _jsonrpcPath)) {

					// Now dig into the message, if the method is for the controller and the method is to get info
                    // it is good to go... Other wise NO
                    Core::ProxyType<const Core::JSONRPC::Message> body = request.Body<const Core::JSONRPC::Message>();

					if (body.IsValid() == true)
					{
                        result = CheckMessage(*body);
					}	
				}
            }
            return (result);
        }
        //! Allow a JSONRPC message to be checked before it is offered for processing.
		virtual bool Allowed(const Core::JSONRPC::Message& message) const override {
            return ((_hasSecurity == false) || CheckMessage(message));
        }

        //  IUnknown methods
        // -------------------------------------------------------------------------------------------------------
        BEGIN_INTERFACE_MAP(DefaultSecurity)
        INTERFACE_ENTRY(ISecurity)
        END_INTERFACE_MAP

	private:
        bool CheckMessage(const Core::JSONRPC::Message& message) const
        {
            bool result = false;

            if (message.Callsign() == _controllerName)
            {
				result = (message.Method() == _T("exists"));
            }

			return (result);
		}

    private:
        bool _hasSecurity;
        const string _controllerPath;
        const string _jsonrpcPath;
        const string _controllerName;
    };

    static Core::NodeId DetermineAccessor(const Server::Config& configuration, Core::NodeId& accessor)
    {
        Core::NodeId result(configuration.Binding.Value().c_str());

        if (configuration.Interface.Value().empty() == false) {
            Core::NodeId selectedNode = Plugin::Config::IPV4UnicastNode(configuration.Interface.Value());

            if (selectedNode.IsValid() == true) {
                accessor = selectedNode;
                result = accessor;
            }
        } else if (result.IsAnyInterface() == true) {
            Core::NodeId selectedNode = Plugin::Config::IPV4UnicastNode(configuration.Interface.Value());

            if (selectedNode.IsValid() == true) {
                accessor = selectedNode;
            }
        } else {
            accessor = result;
        }

        if (accessor.IsValid() == false) {

            // Let's go fr the default and make the best of it :-)
            struct sockaddr_in value;

            value.sin_addr.s_addr = 0;
            value.sin_family = AF_INET;
            value.sin_port = htons(configuration.Port.Value());

            result = value;

            TRACE_L1("Invalid config information could not resolve to a proper IP set to: (%s:%d)", result.HostAddress().c_str(), result.PortNumber());
        } else {
            result.PortNumber(configuration.Port.Value());

            string URL(_T("http://"));

            URL += accessor.HostAddress();
            URL += ':' + Core::NumberType<uint32_t>(configuration.Port.Value()).Text();

            accessor.PortNumber(configuration.Port.Value());

            SYSLOG(Logging::Startup, (_T("Accessor: %s"), URL.c_str()));
            SYSLOG(Logging::Startup, (_T("Interface IP: %s"), result.HostAddress().c_str()));
        }

        return (result);
    }

    void Server::ChannelMap::GetMetaData(Core::JSON::ArrayType<MetaData::Channel> & metaData) const
    {

        Core::SocketServerType<Channel>::Iterator index(Core::SocketServerType<Channel>::Clients());

        while (index.Next() == true) {

            MetaData::Channel newInfo;
            // Let the Channel,report it's metadata

            Core::ProxyType<Channel> client(index.Client());

            newInfo.ID = client->Id();

            newInfo.Activity = client->HasActivity();
            newInfo.Remote = client->RemoteId();
            newInfo.JSONState = (client->IsWebSocket() ? ((client->State() != PluginHost::Channel::RAW) ? MetaData::Channel::RAWSOCKET : MetaData::Channel::WEBSOCKET) : (client->IsWebServer() ? MetaData::Channel::WEBSERVER : MetaData::Channel::SUSPENDED));
            string name = client->Name();

            if (name.empty() == false) {
                newInfo.Name = name;
            }

            metaData.Add(newInfo);
        }
    }

    void Server::ServiceMap::Destroy()
    {
        _adminLock.Lock();

        std::map<const string, Core::ProxyType<Service>>::iterator index(_services.end());

        TRACE_L1("Deactivating %d plugins.", static_cast<uint32_t>(_services.size()));

        // First, move them all to deactivated
        do {
            index--;

            ASSERT(index->second.IsValid());

            index->second->Deactivate(PluginHost::IShell::SHUTDOWN);

        } while (index != _services.begin());

        TRACE_L1("Destructing %d plugins.", static_cast<uint32_t>(_services.size()));
        // Now release them all, once they are deactivated..
        index = _services.begin();

        while (index != _services.end()) {
            Core::ProxyType<Service> service(index->second);

            ASSERT(service.IsValid());

            index = _services.erase(index);

            service.Release();
        }

        Core::ServiceAdministrator::Instance().FlushLibraries();

        _adminLock.Unlock();

        ASSERT(_notifiers.size() == 0);

        std::list<uint32_t> pidList;

        // See if there are still pending processes, we need to shoot and kill..
        Processes(pidList);

        if (pidList.size() > 0) {
            uint16_t waitSlots = 50; /* each slot is 100ms, so we wait for 5 Seconds for all processes to terminte !!! */

            std::list<uint32_t>::iterator index(pidList.begin());

            while (index != pidList.end()) {
                _processAdministrator.Destroy(*index);

                SYSLOG(Logging::Shutdown, (_T("Process [%d] requested to be shutdown."), *index));

                index++;
            }

            // Now it is time to wait, for a certain amount of time to see
            // if all processes are killed.
            while ((waitSlots-- != 0) && (pidList.size() > 0)) {
                SleepMs(100);

                // Now chek the process in the list if they are still alive..
                std::list<uint32_t>::iterator check(pidList.begin());

                while (check != pidList.end()) {
                    Core::ProcessInfo process(*check);

                    if (process.IsActive() == false) {
                        check = pidList.erase(check);
                    } else {
                        check++;
                    }
                }
            }

            if (pidList.size() > 0) {
                index = pidList.begin();

                while (index != pidList.end()) {
                    SYSLOG(Logging::Shutdown, (_T("Process [%d] could not be destroyed in time."), *index));
                    index++;
                }
            }
        }
    }

    /* virtual */ void* Server::Service::QueryInterface(const uint32_t id)
    {

        void* result = nullptr;
        if ((id == Core::IUnknown::ID) || (id == PluginHost::IShell::ID)) {
            AddRef();
            result = this;
        } else {

            Lock();

            if (_handler != nullptr) {

                result = _handler->QueryInterface(id);
            }

            Unlock();
        }

        return (result);
    }

    /* virtual */ void* Server::Service::QueryInterfaceByCallsign(const uint32_t id, const string& name)
    {
        return (_administrator.QueryInterfaceByCallsign(id, name));
    }

    /* virtual */ void Server::Service::Register(IPlugin::INotification * sink)
    {
        _administrator.Register(sink);
    }

    /* virtual */ void Server::Service::Unregister(IPlugin::INotification * sink)
    {
        _administrator.Unregister(sink);
    }

    // Use the base framework (webbridge) to start/stop processes and the service in side of the given binary.
    /* virtual */ PluginHost::IShell::IProcess* Server::Service::Process()
    {
        return (&_administrator);
    }

    // Methods to stop/start/update the service.
    uint32_t Server::Service::Activate(const PluginHost::IShell::reason why)
    {

        uint32_t result = Core::ERROR_NONE;

        Lock();

        IShell::state currentState(State());

        if (currentState == IShell::ACTIVATION) {
            result = Core::ERROR_INPROGRESS;
        } else if ((currentState == IShell::DEACTIVATION) || (currentState == IShell::DESTROYED)) {
            result = Core::ERROR_ILLEGAL_STATE;
        } else if ((currentState == IShell::DEACTIVATED) || (currentState == IShell::PRECONDITION)) {

            // Load the interfaces, If we did not load them yet...
            if (_handler == nullptr) {
                AquireInterfaces();
            }

            const string callSign(PluginHost::Service::Configuration().Callsign.Value());
            const string className(PluginHost::Service::Configuration().ClassName.Value());

            if (_handler == nullptr) {
                SYSLOG(Logging::Startup, (_T("Activation of plugin [%s]:[%s], failed. Error [%s]"), className.c_str(), callSign.c_str(), ErrorMessage().c_str()));
                result = Core::ERROR_UNAVAILABLE;

                // See if the preconditions have been met..
            } else if (_precondition.IsMet() == false) {
                SYSLOG(Logging::Startup, (_T("Activation of plugin [%s]:[%s], postponed, preconditions have not been met, yet."), className.c_str(), callSign.c_str()));
                result = Core::ERROR_PENDING_CONDITIONS;
                _reason = why;
                State(PRECONDITION);

                if (Trace::TraceType<Activity, &Core::System::MODULE_NAME>::IsEnabled() == true) {
                    string feedback;
                    uint8_t index = 1;
                    uint32_t delta(_precondition.Delta(_administrator.SubSystemInfo()));

                    while (delta != 0) {
                        if ((delta & 0x01) != 0) {
                            if (feedback.empty() == false) {
                                feedback += ',';
                            }

                            PluginHost::ISubSystem::subsystem element(static_cast<PluginHost::ISubSystem::subsystem>(index));
                            feedback += string(Core::EnumerateType<PluginHost::ISubSystem::subsystem>(element).Data());
                        }

                        delta = (delta >> 1);
                        index++;
                    }

                    Activity information(_T("Delta preconditions: %s"), feedback.c_str());
                    Trace::TraceType<Activity, &Core::System::MODULE_NAME> traceData(information);
                    Trace::TraceUnit::Instance().Trace(__FILE__, __LINE__, className.c_str(), &traceData);
                }
            } else {

                State(ACTIVATION);
                _administrator.StateChange(this);

                Unlock();

                TRACE(Activity, (_T("Activation plugin [%s]:[%s]"), className.c_str(), callSign.c_str()));

                // Fire up the interface. Let it handle the messages.
                ErrorMessage(_handler->Initialize(this));

                if (HasError() == true) {
                    result = Core::ERROR_GENERAL;

                    SYSLOG(Logging::Startup, (_T("Activation of plugin [%s]:[%s], failed. Error [%s]"), className.c_str(), callSign.c_str(), ErrorMessage().c_str()));

                    Lock();
                    ReleaseInterfaces();
                    State(DEACTIVATED);
                    _administrator.StateChange(this);
                } else {
                    const string webUI(PluginHost::Service::Configuration().WebUI.Value());
                    if ((PluginHost::Service::Configuration().WebUI.IsSet()) || (webUI.empty() == false)) {
                        EnableWebServer(webUI, EMPTY_STRING);
                    }

                    IDispatcher* dispatcher = _handler->QueryInterface<IDispatcher>();

                    if (dispatcher != nullptr) {
                        dispatcher->Activate(this);
                        dispatcher->Release();
                    }

                    SYSLOG(Logging::Startup, (_T("Activated plugin [%s]:[%s]"), className.c_str(), callSign.c_str()));
                    Lock();
                    State(ACTIVATED);
                    _administrator.StateChange(this);
                    _administrator.Notification(_T("{\"callsign\":\"") + callSign + _T("\",\"state\":\"activated\",\"reason\":\"") + IShell::ToString(why) + _T("\"}"));

                    IStateControl* stateControl = nullptr;
                    if ((Resumed() == true) && ((stateControl = _handler->QueryInterface<PluginHost::IStateControl>()) != nullptr)) {

                        stateControl->Request(PluginHost::IStateControl::RESUME);
                        stateControl->Release();
                    }
                }
            }
        }

        Unlock();

        return (result);
    }

    uint32_t Server::Service::Deactivate(const reason why)
    {

        uint32_t result = Core::ERROR_NONE;

        Lock();

        IShell::state currentState(State());

        if (currentState == IShell::DEACTIVATION) {
            result = Core::ERROR_INPROGRESS;
        } else if ((currentState == IShell::ACTIVATION) || (currentState == IShell::DESTROYED)) {
            result = Core::ERROR_ILLEGAL_STATE;
        } else if ((currentState == IShell::ACTIVATED) || (currentState == IShell::PRECONDITION)) {

            ASSERT(_handler != nullptr);

            const string className(PluginHost::Service::Configuration().ClassName.Value());
            const string callSign(PluginHost::Service::Configuration().Callsign.Value());

            _reason = why;

            if (currentState == IShell::ACTIVATED) {
                State(DEACTIVATION);
                _administrator.StateChange(this);

                Unlock();

                // If we enabled the webserver, we should also disable it.
                if ((PluginHost::Service::Configuration().WebUI.IsSet()) || (PluginHost::Service::Configuration().WebUI.Value().empty() == false)) {
                    DisableWebServer();
                }

                TRACE(Activity, (_T("Deactivation plugin [%s]:[%s]"), className.c_str(), callSign.c_str()));

                _handler->Deinitialize(this);

                Lock();

                PluginHost::IDispatcher* dispatcher = dynamic_cast<PluginHost::IDispatcher*>(_handler);

                if (dispatcher != nullptr) {
                    dispatcher->Deactivate();
                }
            }

            SYSLOG(Logging::Shutdown, (_T("Deactivated plugin [%s]:[%s]"), className.c_str(), callSign.c_str()));

            TRACE(Activity, (Trace::Format(_T("Deactivate plugin [%s]:[%s]"), className.c_str(), callSign.c_str())));

            State(DEACTIVATED);

            _administrator.StateChange(this);
            _administrator.Notification(_T("{\"callsign\":\"") + callSign + _T("\",\"state\":\"deactivated\",\"reason\":\"") + IShell::ToString(why) + _T("\"}"));

            if (State() != ACTIVATED) {
                // We have no need for his module anymore..
                ReleaseInterfaces();
            }
        }

        Unlock();

        return (result);
    }

    /* virtual */ uint32_t Server::Service::Submit(const uint32_t id, const Core::ProxyType<Core::JSON::IElement>& response)
    {
        return (_administrator.Submit(id, response));
    }

    /* virtual */ ISubSystem* Server::Service::SubSystems()
    {
        return (_administrator.SubSystemsInterface());
    }

    /* virtual */ void Server::Service::Notify(const string& message)
    {
        const string fullMessage("{\"callsign\":\"" + PluginHost::Service::Callsign() + "\", \"data\": " + message + " }");

        // Notify the base class and the subscribers
        PluginHost::Service::Notification(message);

        _administrator.Notification(fullMessage);
    }

    uint32_t Server::ServiceMap::FromLocator(const string& identifier, Core::ProxyType<PluginHost::Server::Service>& service, bool& serviceCall)
    {
        uint32_t result = Core::ERROR_BAD_REQUEST;
        const string& serviceHeader(_webbridgeConfig.WebPrefix());
        const string& JSONRPCHeader(_webbridgeConfig.JSONRPCPrefix());

        // Check the header (prefix part)
        if (identifier.compare(0, serviceHeader.length(), serviceHeader.c_str()) == 0) {

            serviceCall = true;

            if (identifier.length() <= (serviceHeader.length() + 1)) {
                service = _server._controller;
                result = Core::ERROR_NONE;
            } else {
                size_t length;
                uint32_t offset = static_cast<uint32_t>(serviceHeader.length()) + 1; /* skip the slash after */

                const string callSign(identifier.substr(offset, ((length = identifier.find_first_of('/', offset)) == string::npos ? string::npos : length - offset)));

                result = FromIdentifier(callSign, service);
            }
        } else if (identifier.compare(0, JSONRPCHeader.length(), JSONRPCHeader.c_str()) == 0) {

            serviceCall = false;

            if (identifier.length() <= (JSONRPCHeader.length() + 1)) {
                service = _server._controller;
                result = Core::ERROR_NONE;
            } else {
                size_t length;
                uint32_t offset = static_cast<uint32_t>(JSONRPCHeader.length()) + 1; /* skip the slash after */

                const string callSign(identifier.substr(offset, ((length = identifier.find_first_of('/', offset)) == string::npos ? string::npos : length - offset)));

                result = FromIdentifier(callSign, service);
            }
        }

        return (result);
    }

    Server::Channel::Channel(const SOCKET& connector, const Core::NodeId& remoteId, Core::SocketServerType<Channel>* parent)
        : PluginHost::Channel(connector, remoteId)
        , _parent(static_cast<ChannelMap&>(*parent).Parent())
        , _security(nullptr)
        , _service()
    {
        TRACE(Activity, (_T("Construct a link with ID: [%d] to [%s]"), Id(), remoteId.QualifiedName().c_str()));
    }

    /* virtual */ Server::Channel::~Channel()
    {
        TRACE(Activity, (_T("Destruct a link with ID [%d] to [%s]"), Id(), RemoteId().c_str()));

        // If we are still atatched to a service, detach, we are out of scope...
        if (_service.IsValid() == true) {
            _service->Unsubscribe(*this);

            _service.Release();
        }
        if (_security != nullptr) {
            _security->Release();
            _security = nullptr;
        }

        Close(0);
    }

    static string DetermineProperModel(Core::JSON::String & input)
    {
        string result;

        if (input.IsSet()) {
            result = input.Value();
        } else if (Core::SystemInfo::GetEnvironment(_T("MODEL_NAME"), result) == false) {
            result = "UNKNOWN";
        }
        return (result);
    }

#ifdef __WIN32__
#pragma warning(disable : 4355)
#endif

    Server::Server(Server::Config & configuration, const bool background)
        : _accessor()
        , _dispatcher(configuration.Process.IsSet() ? configuration.Process.StackSize.Value() : 0)
        , _connections(*this, DetermineAccessor(configuration, _accessor), configuration.IdleTime)
        , _config(configuration.Version.Value(),
              DetermineProperModel(configuration.Model),
              background,
              configuration.Prefix.Value(),
              configuration.JSONRPC.Value(),
              configuration.VolatilePath.Value(),
              configuration.PersistentPath.Value(),
              configuration.DataPath.Value(),
              configuration.SystemPath.Value(),
              configuration.ProxyStubPath.Value(),
              configuration.Signature.Value(),
              _accessor,
              Core::NodeId(configuration.Communicator.Value().c_str()),
              configuration.Redirect.Value())
        , _services(*this, _config, configuration.Process.IsSet() ? configuration.Process.StackSize.Value() : 0)
        , _controller()
    {

        // See if the persitent path for our-selves exist, if not we will create it :-)
        Core::File persistentPath(_config.PersistentPath() + _T("PluginHost"));

        if (persistentPath.IsDirectory() == false) {
            Core::Directory(persistentPath.Name().c_str()).Create();
        }

        Core::JSON::ArrayType<Plugin::Config>::Iterator index = configuration.Plugins.Elements();

        // First register all services, than if we got them, start "activating what is required.
        // Whatever plugin is needed, we at least have our MetaData plugin available (as the first entry :-).
        Plugin::Config metaDataConfig;

        metaDataConfig.ClassName = Core::ClassNameOnly(typeid(Plugin::Controller).name()).Text();

        while (index.Next() == true) {
            Plugin::Config& entry(index.Current());

            if ((entry.ClassName.Value().empty() == true) && (entry.Locator.Value().empty() == true)) {

                // This is a definition/configuration for the Controller or an incorrect entry :-).
                // Read and define the Controller.
                if (metaDataConfig.Callsign.Value().empty() == true) {
                    // Oke, this is the first time we "initialize" it.
                    metaDataConfig.Callsign = (entry.Callsign.Value().empty() == true ? string(_defaultControllerCallsign) : entry.Callsign.Value());
                    metaDataConfig.Configuration = entry.Configuration;
                } else {
                    // Let's raise an error, this is a bit strange, again, the controller is initialized !!!
#ifndef __WIN32__
                    if (background == true) {
                        syslog(LOG_NOTICE, "Configuration error. Controller is defined mutiple times [%s].\n", entry.Callsign.Value().c_str());
                    } else
#endif
                    {
                        fprintf(stdout, "Configuration error. Controller is defined mutiple times [%s].\n", entry.Callsign.Value().c_str());
                    }
                }
            } else {
                _services.Insert(entry);
            }
        }

        if (metaDataConfig.Callsign.Value().empty() == true) {
            // Oke, this is the first time we "initialize" it.
            metaDataConfig.Callsign = string(_defaultControllerCallsign);
        }

        // Get the configuration from the persistent location.
        _services.Load();

        // Create input handle
        _inputHandler.Initialize(configuration.Input.Type.Value(), configuration.Input.Locator.Value());

        // Initialize static message.
        Service::Initialize();
        Channel::Initialize(_config.WebPrefix());

        // Add the controller as a service to the services.
        _controller = _services.Insert(metaDataConfig);
    }

#ifdef __WIN32__
#pragma warning(default : 4355)
#endif

    Server::~Server()
    {
    }

    void Server::Open()
    {
        // Before we do anything with the subsystems (notifications)
        // Lets see if security is already set..
        DefaultSecurity* securityProvider = 
			Core::Service<DefaultSecurity>::Create<DefaultSecurity>(
                _config.WebPrefix(),
                _config.JSONRPCPrefix(),
                _controller->Callsign());

        _config.Security(securityProvider);

        _controller->Activate(PluginHost::IShell::STARTUP);

		if ((_services.SubSystemInfo() & (1 << ISubSystem::SECURITY)) == 0) {
			// The controller is on control of the security, so I guess all systems green
			// as the controller does not know anything about security :-)
            securityProvider->Security(false);
        }

		securityProvider->Release();
        
        _controller->ClassType<Plugin::Controller>()->SetServer(this);
        _controller->ClassType<Plugin::Controller>()->AddRef();


        // Right we have the shells for all possible services registered, time to activate what is needed :-)
        ServiceMap::Iterator iterator(_services.Services());

        while (iterator.Next() == true) {

            Core::ProxyType<Service> service(*iterator);

            if (service->AutoStart() == true) {

                _dispatcher.Submit(PluginHost::IShell::Job::Create(&(*service), PluginHost::IShell::ACTIVATED, PluginHost::IShell::STARTUP));
            } else {
                SYSLOG(Logging::Startup, (_T("Activation of plugin [%s]:[%s] blocked"), service->ClassName().c_str(), service->Callsign().c_str()));
            }
        }
        Dispatcher().Open(MAX_EXTERNAL_WAITS);
    }

    void Server::Close()
    {
        Plugin::Controller* destructor(_controller->ClassType<Plugin::Controller>());
        _dispatcher.Block();
        _connections.Close(Core::infinite);
        destructor->Stopped();
        _services.Destroy();
        destructor->Release();
        _inputHandler.Deinitialize();
        _dispatcher.Wait(Core::Thread::BLOCKED | Core::Thread::STOPPED, Core::infinite);
    }
}
}
