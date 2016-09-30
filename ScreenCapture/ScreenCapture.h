#pragma once

#include <memory>

namespace screencast {

class ScreenCapturer
{
public:
	virtual ~ScreenCapturer() {}
	static std::shared_ptr<ScreenCapturer> Instance();
	virtual bool Capture() = 0;
};

} // namespace screencast
