//
// Created by Cristobal Mendoza on 2/18/20.
//

#include "ofxParameterServer.hpp"

//const std::string ModelRemoteController::ApiRoot = "mtafMethod";

ofxParameterServer::ofxParameterServer() : ofThread()
{
	ipAddress = "";

	addServerMethod(ServerMethod(
			"getModel",
			"Get model",
			[this](ServerMethod& method, ofxOscMessage& m, ofxParameterServer& server)
			{
				ofxOscMessage outMessage;
				auto xml = createMetaModel();
				outMessage.addStringArg(xml.toString());
				outMessage.setRemoteEndpoint(m.getRemoteHost(), outPort);
				server.sendReply(method, outMessage);
			}));

	addServerMethod(ServerMethod(
			"connect",
			"Connect",
			[this](ServerMethod& method, ofxOscMessage& m, ofxParameterServer& server)
			{
				oscSender.setup(m.getArgAsString(0), outPort);
				ofxOscMessage outMessage;
				outMessage.addStringArg("OK");
				server.sendReply(method, outMessage);
			}));

	addServerMethod(ServerMethod(
			"set",
			"Set parameter",
			[this](ServerMethod& method, ofxOscMessage& m, ofxParameterServer& server)
			{
				if (m.getNumArgs() != 2)
				{
					ofLogError("ModelServer")
							<< "Tried to set parameter but OSC Message did not have the right number of arguments.";
					return;
				}
				ParameterData pd;
				pd.parameterPath = m.getArgAsString(0);
				pd.parameterValue = m.getArgAsString(1);
				parameterThreadChannel.send(std::move(pd));
//				setParameter(m.getArgAsString(0), m.getArgAsString(1));
			}));


	// We can add here any value that OF has an ofToString for.
	// Any other types you'll need to add externally
	addParameterType<int>("int", true);
	addParameterType<float>("float", true);
	addParameterType<double>("double", true);
	addParameterType<ofFloatColor>("floatColor");
	addParameterType<ofParameterGroup>("group");
	addParameterType<std::string>("string");
	addParameterType<bool>("boolean");
	addParameterType<glm::vec2>("vec2", true);
	addParameterType<glm::vec3>("vec3", true);
	addParameterType<glm::vec4>("vec4", true);
	addParameterType<ofRectangle>("ofRectangle");
	addParameterType<ofQuaternion>("ofQuaternion");
	addParameterType<ofMatrix3x3>("ofMatrix3x3");
	addParameterType<ofMatrix4x4>("ofMatrix4x4");

	// Add ofParameterGroup "manually" because we don't want it boxed into an ofParameter:
	auto result = typeRegistry
			.insert({std::type_index(typeid(ofParameterGroup)), {.friendlyName = "group", .hasLimits = false}});

}

ofxParameterServer::~ofxParameterServer()
{
	waitForThread();
	oscReceiver.stop();
	oscSender.clear();
}

void ofxParameterServer::setup(ofParameterGroup& parameters,
							   std::string remoteHost,
							   int inPort,
							   int outPort)
{
	group = std::make_shared<ofParameterGroup>(parameters);
	oscReceiver.setup(inPort);
	this->inPort = inPort;
	this->outPort = outPort;

	// Todo:
	// Add a listener to all parameters so that when any of them updates we
	// can send the updated model to the OSC Remote
	// Also TODO:
	// Implement mechanism for server-initiated data transmission to the Remote
	// But how do we avoid infinite "feedback"?
	startThread();

	// Call update on the OF loop event:
	loopListener = ofGetMainLoop()->loopEvent.newListener([this]() {
		update();
	});
}

/**
 * @brief The server's setParameter method needs to be called on the main thread, which is achieved
 * by calling ofxParameterServer::update().
 */
void ofxParameterServer::update()
{

	std::unique_lock<std::mutex> lock(serverMutex);
	std::unordered_map<std::string, std::string> pDataMap;
	ParameterData parameterData;
	while (parameterThreadChannel.tryReceive(parameterData))
	{
		pDataMap[parameterData.parameterPath] = parameterData.parameterValue;
//		setParameter(parameterData.parameterPath, parameterData.parameterValue);
	}
	// Only send the last received data for a given path... otherwise things get way too spammy
	for (auto& pair : pDataMap)
	{
		setParameter(pair.first, pair.second);
	}
}

ofXml ofxParameterServer::createMetaModel()
{
	ofXml xml;
	auto root = xml.appendChild("ParameterServer");
	auto paramsXml = root.appendChild("Parameters");
	serializeParameterGroup(group, paramsXml);
	auto methodsXml = root.appendChild("Methods");
	serializeMethods(methodsXml);
	return xml;
}

void ofxParameterServer::serializeMethods(ofXml& xml)
{
	for (auto& pair : serverMethods)
	{
		xml.appendChild(pair.second.getIdentifier())
		   .setAttribute("uiName", pair.second.getUiName());
	}
}

void ofxParameterServer::serializeParameterGroup(std::shared_ptr<ofParameterGroup> params, ofXml& xml)
{
	auto groupXml = xml.appendChild(params->getEscapedName());
	groupXml.appendAttribute("type").set("group"); // TODO: type string constants?
	groupXml.appendAttribute("name").set(params->getName());
	for (auto& param : *params)
	{
		serializeParameter(param, groupXml);
	}
}

void ofxParameterServer::serializeParameter(std::shared_ptr<ofAbstractParameter> parameter, ofXml& xml)
{
	TypeInfo typeInfo;
	std::string min = "";
	std::string max = "";

	try
	{
		typeInfo = typeRegistry.at(std::type_index(typeid(*parameter)));
	}
	catch (std::out_of_range e)
	{
		ofLogNotice("ModelServer") << "Tried adding parameter of unknown type: "
								  << typeid(*parameter).name()
								  << " Register the type with addType before adding such a parameter.";
		return;
	}

	auto parameterGroup = std::dynamic_pointer_cast<ofParameterGroup>(parameter);
	if (parameterGroup)
	{
		serializeParameterGroup(parameterGroup, xml);
		return;
	}

	ofXml paramXml = xml.appendChild(parameter->getEscapedName());

	if (typeInfo.hasLimits)
	{
		auto limits = typeInfo.getLimits(parameter);
		min = limits.first;
		max = limits.second;
	}

	paramXml.appendAttribute("type").set(typeInfo.friendlyName);
	paramXml.appendAttribute("name").set(parameter->getName());
	paramXml.appendChild("value").set(parameter->toString());
	if (min != max)
	{
		paramXml.appendChild("min").set(min);
		paramXml.appendChild("max").set(max);
	}

}

/**
 * @brief Sets the value of the parameter at the path.
 * @param path The path for the ofParameter in the ofParameterGroup hierarchy
 * @param value The value for the parameter in string form
 */
void ofxParameterServer::setParameter(std::string path, std::string value)
{
	auto pathComponents = ofSplitString(path, "/", true, true);
	if (pathComponents.size() < 2)
	{
		ofLogError("ModelServer::setParameter") << "setParameter: path is too short. Path: "
												<< path;
		return;
	}

	auto current = group;

	// Basic check

	if (current->getEscapedName() != pathComponents[0])
	{
		ofLogError("ModelServer::setParameter") << "Something went wrong";
		return;
	}

	for (int i = 1; i < pathComponents.size() - 1; i++)
	{
		std::string pc = pathComponents[i];

		auto result = findParamWithEscapedName(*current, pc);

		if (result.first)
		{
			auto parameterGroup = std::dynamic_pointer_cast<ofParameterGroup>(result.second);

			if (parameterGroup)
			{
				current = parameterGroup;
			}
		}
		else
		{
			ofLogVerbose("ModelServer::setParameter")
					<< "Couldn't find parameter group. Trying custom deserializer";
			// Try a custom deserializer here?
			useCustomDeserializer(path, value);
			return;

		}
	}

	// If we are still here we have the group with the parameter
	auto result = findParamWithEscapedName(*current, pathComponents.back());
	if (result.first)
	{
		result.second->fromString(value);
	}
	else
	{
		// Try a custom deserializer here?
		useCustomDeserializer(path, value);
	}
}

bool ofxParameterServer::useCustomDeserializer(const std::string& path, const std::string& value) const
{
	auto iter = customDeserializers.find(path);
	if (iter != customDeserializers.end())
	{
		iter->second(path + "/" + value);
		return true;
	}
	else
	{
		ofLogError("ModelServer::setParameter") << "Couldn't deserialize parameter: "
												<< path << "/" << value;
		return false;
	}
}

std::pair<bool, std::shared_ptr<ofAbstractParameter>>
ofxParameterServer::findParamWithEscapedName(ofParameterGroup& groupToSearch, std::string escapedName)
{
	auto iter = std::find_if(groupToSearch.begin(),
							 groupToSearch.end(),
							 [&escapedName](std::shared_ptr<ofAbstractParameter> param)
							 {
								 if (param->getEscapedName() == escapedName)
								 {
									 return true;
								 }
								 return false;
							 });
	if (iter != groupToSearch.end())
	{
		return std::make_pair(true, *iter);
	}
	else
	{
		return std::make_pair(false, nullptr);
	}
}

bool ofxParameterServer::sendMetaModel()
{
	return false;
}

void ofxParameterServer::parseMessage(ofxOscMessage& m)
{
	ofLogVerbose("ofxParameterServer") << "address = " << m.getAddress() << "\n";
	ofLogVerbose("ofxParameterServer") << "arg = " << m.getArgAsString(0) << "\n";
	for (int i = 0; i < m.getNumArgs(); i++)
	{
		ofLogVerbose("ofxParameterServer") << "arg " << i << " = " << m.getArgAsString(i) << "\n";
	}

	auto components = ofSplitString(m.getAddress(),
									ofToString((char) std::filesystem::path::preferred_separator),
									false,
									true);
	for (int j = 0; j < components.size(); ++j)
	{
		ofLogVerbose("ofxParameterServer") << j << " " << components[j] << "\n";
	}

	if (components.size() < 3) return;

	if (components[1] == ApiRoot)
	{
		auto result = serverMethods.find(components[2]);
		if (result != serverMethods.end())
		{
			result->second.execute(m, *this);
		}
	}
}

void ofxParameterServer::threadedFunction()
{
	while (isThreadRunning())
	{
		serverMutex.lock();
		while (oscReceiver.hasWaitingMessages())
		{
			ofxOscMessage inMsg;
			oscReceiver.getNextMessage(inMsg);
			parseMessage(inMsg);
		}
		serverMutex.unlock();
		sleep(16);
//		yield();
	}
}

void ofxParameterServer::sendMessage(ofxOscMessage& m)
{
	oscSender.sendMessage(m, false);
}

void ofxParameterServer::sendReply(ServerMethod& method, ofxOscMessage& m)
{
	m.setAddress("/" + ApiResponse + "/" + method.getIdentifier());
	ofLogVerbose("Sending:") << m;
	oscSender.sendMessage(m, false);
}

void ofxParameterServer::addServerMethod(ServerMethod&& method)
{
	std::unique_lock<std::mutex> lock(serverMutex);
	serverMethods.insert({method.getIdentifier(), method});
}

void ofxParameterServer::addCustomDeserializer(std::string path,
											   std::function<void(std::string)> customDeserializer)
{
	std::unique_lock<std::mutex> lock(serverMutex);
	customDeserializers[path] = customDeserializer;
}
