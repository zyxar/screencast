#pragma once

#include <memory>
#include <string>
#include <deque>

namespace screencast {

class ScreenCapturer
{
public:
	virtual ~ScreenCapturer() {}
	static std::shared_ptr<ScreenCapturer> Instance();
	virtual bool Capture() = 0;
	static std::deque<std::wstring> FileList;
};

} // namespace screencast
