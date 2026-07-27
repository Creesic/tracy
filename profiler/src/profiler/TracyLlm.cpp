#include <array>
#include <cmath>
#include <cstring>
#include <vector>
#include <curl/curl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined( __APPLE__ )
#include <mach-o/dyld.h>
#endif
#endif

#include "TracyConfig.hpp"
#include "TracyFilesystem.hpp"
#include "TracyImGui.hpp"
#include "TracyLlm.hpp"
#include "TracyLlmApi.hpp"
#include "TracyLlmChat.hpp"
#include "TracyLlmTools.hpp"
#include "TracyPrint.hpp"
#include "TracyView.hpp"
#include "TracyWeb.hpp"
#include "TracyWorker.hpp"
#include "../Fonts.hpp"
#include "../public/common/TracySystem.hpp"
#if defined( TRACY_MCP_EMBEDDED )
#include "McpStreamableHttp.hpp"
#endif

#include "data/PersonalityAnnoyed.hpp"
#include "data/PersonalityEmotion.hpp"
#include "data/SystemPrompt.hpp"
#include "data/SkillCallstack.hpp"
#include "data/SkillOptimization.hpp"
#include "data/ToolsJson.hpp"

namespace tracy
{

namespace
{

std::string ParentPath( std::string p )
{
    while( !p.empty() && ( p.back() == '/' || p.back() == '\\' ) ) p.pop_back();
    const auto pos = p.find_last_of( "/\\" );
    if( pos == std::string::npos ) return {};
    return p.substr( 0, pos );
}

std::string GetExecutablePath()
{
#ifdef _WIN32
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA( NULL, buf, MAX_PATH );
    if( n == 0 || n >= MAX_PATH ) return {};
    return std::string( buf, buf + n );
#elif defined( __APPLE__ )
    char buf[4096];
    uint32_t size = sizeof( buf );
    if( _NSGetExecutablePath( buf, &size ) != 0 ) return {};
    return buf;
#else
    char buf[4096];
    const ssize_t n = readlink( "/proc/self/exe", buf, sizeof( buf ) - 1 );
    if( n <= 0 ) return {};
    buf[n] = '\0';
    return buf;
#endif
}

std::string JoinUnder( const std::string& root, const char* rel )
{
    if( root.empty() ) return rel;
    std::string r = root;
    while( !r.empty() && ( r.back() == '/' || r.back() == '\\' ) ) r.pop_back();
    return r + "/" + rel;
}

std::string RepoFromStandardIntegratorJs( const std::string& script )
{
    static const char kTail[] = "mcp/integrator/dist/index.js";
    static const char kTailWin[] = "mcp\\integrator\\dist\\index.js";
    const size_t n1 = sizeof( kTail ) - 1;
    const size_t n2 = sizeof( kTailWin ) - 1;
    if( script.size() >= n1 && script.compare( script.size() - n1, n1, kTail, n1 ) == 0 )
        return ParentPath( script.substr( 0, script.size() - n1 ) );
    if( script.size() >= n2 && script.compare( script.size() - n2, n2, kTailWin, n2 ) == 0 )
        return ParentPath( script.substr( 0, script.size() - n2 ) );
    return {};
}

bool ResolveIntegratorPaths( std::string& outScript, std::string& outRepo, char* err, size_t errLen )
{
    if( !s_config.llmIntegratorScript.empty() )
    {
        outScript = s_config.llmIntegratorScript;
        if( !FileExists( outScript.c_str() ) )
        {
            snprintf( err, errLen, "Integrator script not found:\n%s", outScript.c_str() );
            return false;
        }
        if( !s_config.llmIntegratorTracyRoot.empty() )
        {
            outRepo = s_config.llmIntegratorTracyRoot;
        }
        else
        {
            outRepo = RepoFromStandardIntegratorJs( outScript );
        }
        while( !outRepo.empty() && ( outRepo.back() == '/' || outRepo.back() == '\\' ) ) outRepo.pop_back();
        return true;
    }

    if( !s_config.llmIntegratorTracyRoot.empty() )
    {
        outRepo = s_config.llmIntegratorTracyRoot;
        while( !outRepo.empty() && ( outRepo.back() == '/' || outRepo.back() == '\\' ) ) outRepo.pop_back();
        outScript = JoinUnder( outRepo, "mcp/integrator/dist/index.js" );
        if( !FileExists( outScript.c_str() ) )
        {
            snprintf( err, errLen, "Built integrator not found (run npm run build in mcp/integrator):\n%s", outScript.c_str() );
            return false;
        }
        return true;
    }

    const std::string exe = GetExecutablePath();
    if( exe.empty() )
    {
        snprintf( err, errLen, "Could not locate the profiler executable path; set \"Tracy root\" under Advanced." );
        return false;
    }

    std::string walk = ParentPath( exe );
    for( int i = 0; i < 24 && !walk.empty(); ++i )
    {
        outScript = JoinUnder( walk, "mcp/integrator/dist/index.js" );
        if( FileExists( outScript.c_str() ) )
        {
            outRepo = walk;
            return true;
        }
        const std::string next = ParentPath( walk );
        if( next == walk ) break;
        walk = next;
    }

    snprintf( err, errLen, "Could not find mcp/integrator/dist/index.js next to this profiler. Build it (npm run build in mcp/integrator) or set \"Tracy root\" under Advanced." );
    return false;
}

bool FindNativeMcpBridgeExecutable( std::string& outExe )
{
#ifdef _WIN32
    static const char kBridge[] = "tracy-mcp-bridge.exe";
#else
    static const char kBridge[] = "tracy-mcp-bridge";
#endif
    if( !s_config.llmIntegratorBridge.empty() && FileExists( s_config.llmIntegratorBridge.c_str() ) )
    {
        outExe = s_config.llmIntegratorBridge;
        return true;
    }
    const std::string exep = GetExecutablePath();
    if( exep.empty() ) return false;
    std::string walk = ParentPath( exep );
    for( int i = 0; i < 24 && !walk.empty(); ++i )
    {
        outExe = JoinUnder( walk, kBridge );
        if( FileExists( outExe.c_str() ) ) return true;
#ifdef _WIN32
        outExe = JoinUnder( walk, "mcp/bridge/build/Release/tracy-mcp-bridge.exe" );
        if( FileExists( outExe.c_str() ) ) return true;
        outExe = JoinUnder( walk, "mcp/bridge/build/Debug/tracy-mcp-bridge.exe" );
        if( FileExists( outExe.c_str() ) ) return true;
#else
        outExe = JoinUnder( walk, "mcp/bridge/build/tracy-mcp-bridge" );
        if( FileExists( outExe.c_str() ) ) return true;
#endif
        const std::string next = ParentPath( walk );
        if( next == walk ) break;
        walk = next;
    }
    return false;
}

}

extern double s_time;

constexpr size_t InputBufferSize = 1024;

TracyLlm::TracyLlm( Worker& worker, View& view, const TracyManualData& manual )
    : m_exit( false )
    , m_input( nullptr )
    , m_apiInput( nullptr )
    , m_worker( worker )
    , m_view( view )
{
    SyncIntegratorFieldsFromConfig();
    if( !s_config.llm ) return;

    static bool initialized = false;
    if( !initialized )
    {
        initialized = true;
        curl_global_init( CURL_GLOBAL_ALL );
        atexit( curl_global_cleanup );
    }

    AddSkill( "callstack", "Analyze content of a call stack or crash trace", Unembed( SkillCallstack ) );
    AddSkill( "optimization", "General code optimization workflow", Unembed( SkillOptimization ) );

    m_personality.emplace_back();
    AddPersonality( Unembed( PersonalityEmotion ) );
    AddPersonality( Unembed( PersonalityAnnoyed ) );

    m_systemPrompt = Unembed( SystemPrompt );
    auto toolsJson = Unembed( ToolsJson );
    m_toolsJson = nlohmann::json::parse( toolsJson->data(), toolsJson->data() + toolsJson->size() );

    m_input = new char[InputBufferSize];
    m_apiInput = new char[InputBufferSize];
    *m_input = 0;

    m_api = std::make_unique<TracyLlmApi>();
    m_chatUi = std::make_unique<TracyLlmChat>( view, worker, m_skills );
    m_tools = std::make_unique<TracyLlmTools>( worker, view, manual, m_skills );

    m_busy = true;
    QueueConnect();
    m_thread = std::thread( [this] { WorkerThread(); } );
}

TracyLlm::~TracyLlm()
{
    StopIntegratorMcpProcess();

    delete[] m_input;
    delete[] m_apiInput;

    if( m_thread.joinable() )
    {
        {
            std::lock_guard lock( m_jobsLock );
            if( m_currentJob ) m_currentJob->stop = true;
            m_exit.store( true, std::memory_order_release );
            m_cv.notify_all();
        }
        m_thread.join();
    }
}

void TracyLlm::Draw( WindowConstraints& constraints )
{
    const auto scale = GetScale();
    ImGui::SetNextWindowSize( ImVec2( 400 * scale, 800 * scale ), ImGuiCond_FirstUseEver );
    constraints.Constrain();
    ImGui::Begin( "Tracy Assist", &m_show, ImGuiWindowFlags_NoScrollbar );
    if( ImGui::GetCurrentWindowRead()->SkipItems ) { ImGui::End(); return; }

    if( IsBusy() )
    {
        ImGui::PushFont( g_fonts.normal, FontBig );
        ImGui::Dummy( ImVec2( 0, ( ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeight() * 4 ) * 0.5f ) );
        TextCentered( ICON_FA_HOURGLASS );
        TextCentered( "Please wait…" );
        DrawWaitingDotsCentered( s_time );
        ImGui::PopFont();
        ImGui::Dummy( ImVec2( 0, ImGui::GetTextLineHeight() ) );
        ImGui::PushStyleColor( ImGuiCol_Text, GImGui->Style.Colors[ImGuiCol_TextDisabled] );
        char tmp[InputBufferSize + 32];
        snprintf( tmp, sizeof( tmp ), "Connecting to %s", s_config.llmAddress.c_str() );
        TextCentered( tmp );
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }

    auto& style = ImGui::GetStyle();

    const auto manualEmbeddingsState = m_tools->GetManualEmbeddingsState();
    if( manualEmbeddingsState.inProgress )
    {
        ImGui::PushFont( g_fonts.normal, FontBig );
        ImGui::Dummy( ImVec2( 0, ( ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeight() * 7 ) * 0.5f ) );
        TextCentered( ICON_FA_BOOK_BOOKMARK );
        ImGui::Spacing();
        TextCentered( "Building manual embeddings…" );
        ImGui::Spacing();
        DrawWaitingDotsCentered( s_time );
        ImGui::TextUnformatted( "" );
        ImGui::PopFont();
        const float w = 100 * scale;
        const float ww = ImGui::GetWindowWidth();
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
        ImGui::SetCursorPosX( ( ww - w ) * 0.5f );
        ImGui::ProgressBar( manualEmbeddingsState.progress, ImVec2( w, 0 ), "" );
        ImGui::PopStyleVar();
        ImGui::Spacing();
        char tmp[128];
        snprintf( tmp, sizeof( tmp ), "Progress: %.1f%%", manualEmbeddingsState.progress * 100 );
        TextCentered( tmp );
        ImGui::Spacing();
        const auto sz = ImGui::CalcTextSize( "Cancel" ).x + style.FramePadding.x * 2;
        ImGui::SetCursorPosX( ( ImGui::GetWindowWidth() - sz ) * 0.5f );
        if( ImGui::Button( "Cancel" ) ) m_tools->CancelManualEmbeddings();
        ImGui::End();
        return;
    }

    ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1.f, 1.f, 0.f, 1.0f ) );
    ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped( ICON_FA_TRIANGLE_EXCLAMATION );
    ImGui::PopStyleColor();
    if( ImGui::IsItemHovered() )
    {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted( "Always verify the chat responses, as they may contain incorrect or misleading informations." );
        ImGui::EndTooltip();
    }
    ImGui::SameLine();

    std::lock_guard lock( m_chatLock );
    if( m_chatId.load( std::memory_order_acquire ) == 0 ) ResetChat();

    const auto hasChat = m_chat.size() <= 1 && *m_input == 0;
    if( hasChat ) ImGui::BeginDisabled();
    if( ImGui::Button( ICON_FA_BROOM " Clear chat" ) )
    {
        m_jobsLock.lock();
        if( m_currentJob ) m_currentJob->stop = true;
        m_jobsLock.unlock();
        ResetChat();
    }
    if( hasChat ) ImGui::EndDisabled();
    ImGui::SameLine();
    if( ImGui::Button( ICON_FA_ARROWS_ROTATE " Reconnect" ) )
    {
        std::lock_guard lock( m_jobsLock );
        if( m_currentJob ) m_currentJob->stop = true;
        QueueConnect();
    }

    ImGui::SameLine();
    const bool expand = ImGui::TreeNode( "Settings" );
    constraints.MarkMinWidth();
    if( expand )
    {
        m_jobsLock.lock();
        const auto responding = m_currentJob != nullptr;
        m_jobsLock.unlock();
        if( responding ) ImGui::BeginDisabled();
        ImGui::Spacing();
        ImGui::AlignTextToFramePadding();
        TextDisabledUnformatted( ICON_FA_PLUG " API:" );
        ImGui::SameLine();
        const auto sz = std::min( InputBufferSize-1, s_config.llmAddress.size() );
        memcpy( m_apiInput, s_config.llmAddress.c_str(), sz );
        m_apiInput[sz] = 0;
        ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x );
        bool changed = ImGui::InputTextWithHint( "##api", "http://localhost:8080", m_apiInput, InputBufferSize );
        bool commit = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        if( ImGui::BeginCombo( "##presets", nullptr, ImGuiComboFlags_NoPreview ) )
        {
            struct Preset
            {
                const char* name;
                const char* address;
            };
            constexpr static std::array presets = {
                Preset { "Llama.cpp", "http://localhost:8080" },
                Preset { "LM Studio", "http://localhost:1234" },
            };
            for( auto& preset : presets )
            {
                if( ImGui::Selectable( preset.name ) )
                {
                    memcpy( m_apiInput, preset.address, strlen( preset.address ) + 1 );
                    changed = true;
                    commit = true;
                }
            }
            ImGui::EndCombo();
        }
        if( changed ) s_config.llmAddress = m_apiInput;
        if( commit )
        {
            SaveConfig();
            std::lock_guard lock( m_jobsLock );
            QueueConnect();
        }

        const auto& models = m_api->GetModels();
        ImGui::AlignTextToFramePadding();
        TextDisabledUnformatted( ICON_FA_COMMENTS " Chat model:" );
        ImGui::SameLine();
        if( models.empty() || m_modelIdx < 0 )
        {
            ImGui::TextUnformatted( "No models available" );
        }
        else
        {
            ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x );
            if( ImGui::BeginCombo( "##model", models[m_modelIdx].name.c_str() ) )
            {
                for( size_t i = 0; i < models.size(); ++i )
                {
                    const auto& model = models[i];
                    if( model.embeddings ) continue;
                    if( ImGui::Selectable( model.name.c_str(), i == m_modelIdx ) )
                    {
                        m_modelIdx = i;
                        s_config.llmModel = model.name;
                        SaveConfig();
                    }
                    if( m_modelIdx == i ) ImGui::SetItemDefaultFocus();
                    if( !model.quant.empty() )
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled( "(%s)", model.quant.c_str() );
                    }
                }
                ImGui::EndCombo();
            }
        }

        ImGui::AlignTextToFramePadding();
        TextDisabledUnformatted( ICON_FA_BOOK_BOOKMARK " Embeddings model:" );
        ImGui::SameLine();
        if( models.empty() || m_embedIdx < 0 )
        {
            ImGui::TextUnformatted( "No models available" );
        }
        else
        {
            ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x );
            if( ImGui::BeginCombo( "##embedmodel", models[m_embedIdx].name.c_str() ) )
            {
                for( size_t i = 0; i < models.size(); ++i )
                {
                    const auto& model = models[i];
                    if( !model.embeddings ) continue;
                    if( ImGui::Selectable( model.name.c_str(), i == m_embedIdx ) )
                    {
                        m_embedIdx = i;
                        s_config.llmEmbeddingsModel = model.name;
                        SaveConfig();
                        m_tools->SelectManualEmbeddings( model.name );
                    }
                    if( m_embedIdx == i ) ImGui::SetItemDefaultFocus();
                    if( !model.quant.empty() )
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled( "(%s)", model.quant.c_str() );
                    }
                }
                ImGui::EndCombo();
            }
        }
        if( responding ) ImGui::EndDisabled();

        ImGui::Separator();

        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled( "Personality:" );
        ImGui::SameLine();
        if( ImGui::RadioButton( ICON_FA_FACE_MEH " Assistant", &s_config.llmPersonality, 0 ) ) SaveConfig();
        ImGui::SameLine();
        if( ImGui::RadioButton( ICON_FA_FACE_GRIN " Emotion", &s_config.llmPersonality, 1 ) ) SaveConfig();
        ImGui::SameLine();
        if( ImGui::RadioButton( ICON_FA_FACE_FROWN " Annoyed", &s_config.llmPersonality, 2 ) ) SaveConfig();
        if( s_config.llmPersonality != m_personalityPrompt )
        {
            ImGui::SameLine();
            TextColoredUnformatted( 0xFF00FFFF, ICON_FA_TRIANGLE_EXCLAMATION );
            TooltipIfHovered( "Start fresh conversation to apply personality change" );
        }
        constraints.MarkMinWidth();

        ImGui::Checkbox( ICON_FA_EARTH_AMERICAS " Internet access", &m_tools->m_netAccess );
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        if( ImGui::Checkbox( ICON_FA_TAG " Annotate call stacks", &s_config.llmAnnotateCallstacks ) )
        {
            SaveConfig();
        }
        constraints.MarkMinWidth();

        if( ImGui::Checkbox( ICON_FA_HAND_POINT_RIGHT " Show summary", &s_config.llmSummary ) )
        {
            SaveConfig();
        }
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        if( ImGui::Checkbox( ICON_FA_COMMENT_DOTS " Chat suggestion", &s_config.llmSuggestion ) )
        {
            SaveConfig();
        }

        if( ImGui::TreeNode( "Advanced" ) )
        {
            if( responding ) ImGui::BeginDisabled();
            ImGui::AlignTextToFramePadding();
            if( ImGui::Checkbox( ICON_FA_BOLT_LIGHTNING " Fast model:", &s_config.llmSeparateFastModel ) ) SaveConfig();
            ImGui::SameLine();
            if( !s_config.llmSeparateFastModel ) ImGui::BeginDisabled();
            if( models.empty() || m_fastIdx < 0 )
            {
                ImGui::TextUnformatted( "No models available" );
            }
            else
            {
                ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x );
                if( ImGui::BeginCombo( "##fastmodel", models[m_fastIdx].name.c_str() ) )
                {
                    for( size_t i = 0; i < models.size(); ++i )
                    {
                        const auto& model = models[i];
                        if( model.embeddings ) continue;
                        if( ImGui::Selectable( model.name.c_str(), i == m_fastIdx ) )
                        {
                            m_fastIdx = i;
                            s_config.llmFastModel = model.name;
                            SaveConfig();
                        }
                        if( m_fastIdx == i ) ImGui::SetItemDefaultFocus();
                        if( !model.quant.empty() )
                        {
                            ImGui::SameLine();
                            ImGui::TextDisabled( "(%s)", model.quant.c_str() );
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            if( !s_config.llmSeparateFastModel ) ImGui::EndDisabled();

            ImGui::Checkbox( ICON_FA_TEMPERATURE_HALF " Temperature", &m_setTemperature );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 40 * scale );
            if( ImGui::InputFloat( "##temperature", &m_temperature, 0, 0, "%.2f" ) ) m_temperature = std::clamp( m_temperature, 0.f, 2.f );
            if( responding ) ImGui::EndDisabled();

            ImGui::Checkbox( ICON_FA_LIGHTBULB " Show all thinking regions", &m_allThinkingRegions );

            if( ImGui::Checkbox( "Limit tool reply size", &s_config.llmLimitToolReplySize ) )
            {
                SaveConfig();
            }
            ImGui::SameLine();
            if( !s_config.llmLimitToolReplySize ) ImGui::BeginDisabled();
            ImGui::SetNextItemWidth( 100 * scale );
            if( ImGui::InputInt( "##maxtoolsizectrl", &s_config.llmMaxToolReplySizeValue ) )
            {
                s_config.llmMaxToolReplySizeValue = std::max( s_config.llmMaxToolReplySizeValue, 1024 );
                SaveConfig();
            }
            ImGui::SameLine();
            ImGui::TextDisabled( "(bytes)" );
            constraints.MarkMinWidth();
            if( !s_config.llmLimitToolReplySize ) ImGui::EndDisabled();
            if( !models.empty() )
            {
                const auto ctxSize = models[m_modelIdx].contextSize;
                const int ctxBasedLimit = TracyLlmTools::CalcCtxBasedLimit( ctxSize );
                if( ctxBasedLimit > 0 )
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled( "(ctx: %d bytes)", ctxBasedLimit);
                }
            }

            char buf[1024];

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( "User agent:" );
            ImGui::SameLine();
            snprintf( buf, sizeof( buf ), "%s", s_config.llmUserAgent.c_str() );
            ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x );
            if( ImGui::InputTextWithHint( "##useragent", "Spoof user agent", buf, sizeof( buf ) ) )
            {
                s_config.llmUserAgent = buf;
                SaveConfig();
            }

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( "Google Search Engine:" );
            ImGui::SameLine();
            snprintf( buf, sizeof( buf ), "%s", s_config.llmSearchIdentifier.c_str() );
            ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize( ICON_FA_HOUSE ).x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x );
            if( ImGui::InputTextWithHint( "##cse", "search identifier", buf, sizeof( buf ) ) )
            {
                s_config.llmSearchIdentifier = buf;
                SaveConfig();
            }
            ImGui::SameLine();
            if( ImGui::Button( ICON_FA_HOUSE "##cse" ) ) OpenWebpage( "https://cse.google.com/cse/create/new" );

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( "Google Search API Key:" );
            ImGui::SameLine();
            snprintf( buf, sizeof( buf ), "%s", s_config.llmSearchApiKey.c_str() );
            ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize( ICON_FA_HOUSE ).x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x );
            if( ImGui::InputTextWithHint( "##csekey", "search API key", buf, sizeof( buf ) ) )
            {
                s_config.llmSearchApiKey = buf;
                SaveConfig();
            }
            ImGui::SameLine();
            if( ImGui::Button( ICON_FA_HOUSE "##csekey" ) ) OpenWebpage( "https://developers.google.com/custom-search/v1/overview" );

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( "Brave Search API Key:" );
            ImGui::SameLine();
            snprintf( buf, sizeof( buf ), "%s", s_config.llmSearchBraveApiKey.c_str() );
            ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize( ICON_FA_HOUSE ).x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x );
            if( ImGui::InputTextWithHint( "##bravekey", "Brave API key", buf, sizeof( buf ) ) )
            {
                s_config.llmSearchBraveApiKey = buf;
                SaveConfig();
            }
            ImGui::SameLine();
            if( ImGui::Button( ICON_FA_HOUSE "##bravekey" ) ) OpenWebpage( "https://brave.com/search/api/" );

            ImGui::TreePop();
        }

        if( ImGui::TreeNode( ICON_FA_NETWORK_WIRED " Integrator MCP" ) )
        {
            ImGui::TextUnformatted( "MCP for external clients. Prefer the native tracy-mcp-bridge (ship next to the app or use a local build) — no Node. Optional: set \"Integrator script\" for the Node mcp/integrator (npm run build)." );
            ImGui::Spacing();

            if( ImGui::Checkbox( "Enable MCP server", &s_config.llmIntegratorMcpEnabled ) )
            {
                SaveConfig();
                if( s_config.llmIntegratorMcpEnabled ) m_integratorNeedStart = true;
                else
                {
                    m_integratorNeedStart = false;
                    StopIntegratorMcpProcess();
                }
            }

            ImGui::SetNextItemWidth( 90 * scale );
            if( ImGui::InputInt( "Port##intmcp", &s_config.llmIntegratorMcpPort ) )
            {
                if( s_config.llmIntegratorMcpPort < 1 ) s_config.llmIntegratorMcpPort = 1;
                if( s_config.llmIntegratorMcpPort > 65535 ) s_config.llmIntegratorMcpPort = 65535;
                SaveConfig();
                if( s_config.llmIntegratorMcpEnabled )
                {
                    StopIntegratorMcpProcess();
                    m_integratorNeedStart = true;
                }
            }

            char mcpUrl[128];
            snprintf( mcpUrl, sizeof( mcpUrl ), "http://127.0.0.1:%d/mcp", s_config.llmIntegratorMcpPort );
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( "MCP URL:" );
            ImGui::SameLine();
            ImGui::TextUnformatted( mcpUrl );
            ImGui::SameLine();
            if( ImGui::SmallButton( ICON_FA_COPY "##copymcpurl" ) ) ImGui::SetClipboardText( mcpUrl );

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if( m_integratorMcpRunning )
            {
                ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.25f, 0.88f, 0.4f, 1.f ) );
                ImGui::Text( "%s Server running", ICON_FA_CIRCLE_CHECK );
                ImGui::PopStyleColor();
            }
            else if( !s_config.llmIntegratorMcpEnabled )
            {
                ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.5f, 0.52f, 0.58f, 1.f ) );
                ImGui::Text( "%s Server off", ICON_FA_CIRCLE );
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.95f, 0.45f, 0.3f, 1.f ) );
                ImGui::Text( "%s Server not running", ICON_FA_TRIANGLE_EXCLAMATION );
                ImGui::PopStyleColor();
            }

            if( m_integratorScriptUsed[0] != '\0' )
            {
                ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
                ImGui::TextWrapped( "Script: %s", m_integratorScriptUsed );
                ImGui::PopStyleColor();
            }

            ImGui::TextWrapped( "%s", m_integratorStatus );

            if( s_config.llmIntegratorMcpEnabled && !m_integratorMcpRunning && ImGui::SmallButton( "Retry start" ) ) m_integratorNeedStart = true;

            if( ImGui::TreeNode( "Advanced (paths)" ) )
            {
                SyncIntegratorFieldsFromConfig();

                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted( "Node executable:" );
                ImGui::SameLine();
                ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x );
                if( ImGui::InputTextWithHint( "##intmcpnode", "node (search PATH) or full path", m_integratorNode, sizeof( m_integratorNode ) ) )
                {
                    s_config.llmIntegratorNode = m_integratorNode;
                    SaveConfig();
                    if( s_config.llmIntegratorMcpEnabled )
                    {
                        StopIntegratorMcpProcess();
                        m_integratorNeedStart = true;
                    }
                }

                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted( "Integrator script (optional if TRACY_ROOT set):" );
                ImGui::SameLine();
                ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x );
                if( ImGui::InputTextWithHint( "##intmcpscript", "…/mcp/integrator/dist/index.js", m_integratorScript, sizeof( m_integratorScript ) ) )
                {
                    s_config.llmIntegratorScript = m_integratorScript;
                    SaveConfig();
                    if( s_config.llmIntegratorMcpEnabled )
                    {
                        StopIntegratorMcpProcess();
                        m_integratorNeedStart = true;
                    }
                }

                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted( "TRACY_ROOT (repo clone):" );
                ImGui::SameLine();
                ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x );
                if( ImGui::InputTextWithHint( "##intmcproot", "Used for default script + bridge detection", m_integratorTracyRoot, sizeof( m_integratorTracyRoot ) ) )
                {
                    s_config.llmIntegratorTracyRoot = m_integratorTracyRoot;
                    SaveConfig();
                    if( s_config.llmIntegratorMcpEnabled )
                    {
                        StopIntegratorMcpProcess();
                        m_integratorNeedStart = true;
                    }
                }

                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted( "TRACY_MCP_BRIDGE (optional):" );
                ImGui::SameLine();
                ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x );
                if( ImGui::InputTextWithHint( "##intmcpbridge", "tracy-mcp-bridge executable", m_integratorBridge, sizeof( m_integratorBridge ) ) )
                {
                    s_config.llmIntegratorBridge = m_integratorBridge;
                    SaveConfig();
                    if( s_config.llmIntegratorMcpEnabled )
                    {
                        StopIntegratorMcpProcess();
                        m_integratorNeedStart = true;
                    }
                }

                ImGui::TreePop();
            }

            ImGui::TreePop();
        }

        ImGui::TreePop();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    if( !m_api->IsConnected() )
    {
        ImGui::PushFont( g_fonts.normal, FontBig );
        ImGui::Dummy( ImVec2( 0, ( ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeight() * 2 ) * 0.5f ) );
        TextCentered( ICON_FA_PLUG_CIRCLE_XMARK );
        TextCentered( "No connection to LLM API" );
        ImGui::PopFont();
        ImGui::End();
        return;
    }

    const auto& models = m_api->GetModels();
    if( models.empty() || m_modelIdx < 0 )
    {
        ImGui::PushFont( g_fonts.normal, FontBig );
        ImGui::Dummy( ImVec2( 0, ( ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeight() * 2 ) * 0.5f ) );
        TextCentered( ICON_FA_WORM );
        ImGui::Spacing();
        TextCentered( "No models available." );
        ImGui::PopFont();
        ImGui::End();
        return;
    }

    if( !manualEmbeddingsState.done || m_embedIdx < 0 || manualEmbeddingsState.model != models[m_embedIdx].name )
    {
        if( m_embedIdx < 0 ) ImGui::BeginDisabled();
        if( ImGui::SmallButton( ICON_FA_BOOK_BOOKMARK " Learn manual" ) )
        {
            m_jobsLock.lock();
            if( m_currentJob ) m_currentJob->stop = true;
            m_jobsLock.unlock();
            m_tools->BuildManualEmbeddings( models[m_embedIdx].name, *m_api );
        }
        if( m_embedIdx < 0 ) ImGui::EndDisabled();
    }

    const auto ctxSize = models[m_modelIdx].contextSize;
    ImGui::Spacing();
    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
    if( ctxSize <= 0 )
    {
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
        ImGui::ProgressBar( 1, ImVec2( -1, 0 ), "" );
    }
    else
    {
        const auto ratio = m_usedCtx / (float)ctxSize;
        if( ratio < 0.5f )
        {
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        }
        else if( ratio < 0.8f )
        {
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.6f, 0.6f, 0.2f, 1.0f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        }
        ImGui::ProgressBar( ratio, ImVec2( -1, 0 ), "" );
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if( ImGui::IsItemHovered() )
    {
        ImGui::BeginTooltip();
        if( ctxSize <= 0 )
        {
            ImGui::TextUnformatted( "Context size is not available" );
        }
        else
        {
            TextFocused( "Used context size:", RealToString( m_usedCtx ) );
            ImGui::SameLine();
            char buf[64];
            PrintStringPercent( buf, m_usedCtx / (float)ctxSize * 100 );
            tracy::TextDisabledUnformatted( buf );
            TextFocused( "Available context size:", RealToString( ctxSize ) );
            ImGui::Separator();
            tracy::TextDisabledUnformatted( ICON_FA_TRIANGLE_EXCLAMATION " Context use may be an estimate" );
        }
        ImGui::EndTooltip();
    }

    if( !m_summary.empty() )
    {
        TextDisabledUnformatted( ICON_FA_HAND_POINT_RIGHT );
        ImGui::SameLine();
        TextDisabledUnformatted( m_summary.c_str() );
    }

    bool inputChanged = false;
    ImGui::Spacing();
    ImGui::BeginChild( "##chat", ImVec2( 0, -( ImGui::GetFrameHeight() + style.ItemSpacing.y * 2 ) ), ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar );
    if( m_chat.size() <= 1 )   // account for system prompt
    {
        ImGui::Dummy( ImVec2( 0, ( ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeight() * 15 ) * 0.5f ) );
        ImGui::PushStyleColor( ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled] );
        ImGui::PushFont( g_fonts.normal, FontNormal * 3.f );
        TextCentered( ICON_FA_ROBOT );
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::PopFont();
        ImGui::TextWrapped( "What I had not realized is that extremely short exposures to a relatively simple computer program could induce powerful delusional thinking in quite normal people." );
        ImGui::Dummy( ImVec2( 0, ImGui::GetTextLineHeight() * 0.5f ) );
        constexpr auto signature = "-- Joseph Weizenbaum, 1976";
        const auto tw = ImGui::CalcTextSize( signature ).x;
        ImGui::SetCursorPosX( ( ImGui::GetWindowWidth() - tw - 30 * scale ) );
        ImGui::TextUnformatted( signature );
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::PushID( m_chatId.load( std::memory_order_relaxed ) );
        m_chatUi->Begin();

        int thinkIdx = 0;
        if( !m_allThinkingRegions )
        {
            for( thinkIdx = m_chat.size(); thinkIdx > 0; thinkIdx-- )
            {
                const auto& line = m_chat[thinkIdx-1];
                if( !line.contains( "role" ) ) break;
                const auto& roleStr = line["role"].get_ref<const std::string&>();
                if( roleStr == "tool" ) continue;
                if( roleStr == "assistant" && !line.contains( "content" ) ) continue;
                break;
            }
        }

        std::string model;
        uint64_t timeStart = 0;
        uint64_t timeEnd = 0;

        int turnIdx = 0;
        for( auto it = m_chat.begin(); it != m_chat.end(); ++it )
        {
            const auto& line = *it;
            if( !line.contains( "role" ) ) break;
            const auto& roleStr = line["role"].get_ref<const std::string&>();
            if( roleStr == "system" ) continue;

            TracyLlmChat::TurnRole role = TracyLlmChat::TurnRole::None;
            if( roleStr == "user" ) { role = TracyLlmChat::TurnRole::User; timeStart = 0; }
            else if( roleStr == "error" ) { role = TracyLlmChat::TurnRole::Error; timeStart = 0; }
            else if( roleStr == "assistant" || roleStr == "tool" ) role = TracyLlmChat::TurnRole::Assistant;
            else assert( false );

            if( role == TracyLlmChat::TurnRole::User )
            {
                if( line.contains( "content" ) && line["content"].get_ref<const std::string&>().starts_with( "<attachment>\n" ) ) role = TracyLlmChat::TurnRole::Attachment;
            }
            else if( role == TracyLlmChat::TurnRole::Assistant )
            {
                if( timeStart == 0 && line.contains( "model" ) && line.contains( "time_start" ) )
                {
                    model = line["model"].get_ref<const std::string&>();
                    timeStart = line["time_start"].get<uint64_t>();
                }
                if( line.contains( "time_end" ) )
                {
                    timeEnd = line["time_end"].get<uint64_t>();
                    if( timeStart != 0 ) m_chatUi->SetModelTimeLabel( model.c_str(), timeEnd - timeStart );
                }
            }

            ImGui::PushID( turnIdx++ );
            TracyLlmChat::Think think = TracyLlmChat::Think::Hide;
            if( thinkIdx <= turnIdx )
            {
                think = TracyLlmChat::Think::Show;
            }
            else if( thinkIdx == turnIdx + 1 && role == TracyLlmChat::TurnRole::Assistant && line.contains( "content" ) )
            {
                think = TracyLlmChat::Think::ToolCall;
            }
            const auto isLast = it + 1 == m_chat.end();
            bool fadeout = false;
            if( role == TracyLlmChat::TurnRole::Assistant && !m_allThinkingRegions && !isLast )
            {
                auto nit = it + 1;
                while( nit != m_chat.end() )
                {
                    const auto& nline = *nit;
                    if( !nline.contains( "role" ) ) { nit++; continue; }
                    const auto& nroleStr = nline["role"].get_ref<const std::string&>();
                    if( nroleStr == "assistant" && nline.contains( "content" ) )
                    {
                        fadeout = true;
                        break;
                    }
                    if( nroleStr == "user" ) break;
                    nit++;
                }
            }
            if( !m_chatUi->Turn( role, it, m_chat.end(), think, isLast, fadeout ) )
            {
                if( role == TracyLlmChat::TurnRole::Assistant )
                {
                    std::lock_guard lock( m_jobsLock );
                    if( m_currentJob ) m_currentJob->stop = true;
                    QueueSendMessage();
                }
                else if( role == TracyLlmChat::TurnRole::User )
                {
                    if( line.contains( "content" ) )
                    {
                        auto& content = line["content"].get_ref<const std::string&>();
                        const auto sz = std::min( InputBufferSize - 1, content.size() );
                        memcpy( m_input, content.data(), sz );
                        m_input[sz] = 0;
                        inputChanged = true;
                        m_suggestion.clear();
                    }
                }

                auto cit = it;
                while( cit != m_chat.end() )
                {
                    auto& v = *cit;
                    int tokens = 0;
                    int length = 0;
                    if( v.contains( "content" ) )
                    {
                        auto& str = v["content"].get_ref<std::string&>();
                        tokens = m_api->Tokenize( str, m_modelIdx );
                        length = str.size();
                    }
                    if( v.contains( "reasoning_content" ) )
                    {
                        auto& str = v["reasoning_content"].get_ref<std::string&>();
                        tokens += m_api->Tokenize( str, m_modelIdx );
                        length += str.size();
                    }
                    m_usedCtx -= tokens >= 0 ? tokens : length / 4;
                    ++cit;
                }

                m_chat.erase( it, m_chat.end() );
                if( role == TracyLlmChat::TurnRole::User )
                {
                    m_jobsLock.lock();
                    if( m_currentJob ) m_currentJob->stop = true;
                    m_jobsLock.unlock();
                }
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }

        m_chatUi->End();
        ImGui::PopID();

        if( ImGui::GetScrollY() >= ImGui::GetScrollMaxY() )
        {
            ImGui::SetScrollHereY( 1.f );
        }
    }
    ImGui::EndChild();
    ImGui::Spacing();

    m_jobsLock.lock();
    if( m_currentJob )
    {
        const bool disabled = m_currentJob->stop;
        if( disabled ) ImGui::BeginDisabled();
        if( ImGui::Button( ICON_FA_STOP " Stop" ) ) m_currentJob->stop = true;
        m_jobsLock.unlock();
        if( disabled ) ImGui::EndDisabled();
        ImGui::SameLine();
        DrawWaitingDots( s_time );
        ImGui::SameLine();
        if( disabled )
        {
            ImGui::TextUnformatted( "Stopping…" );
        }
        else
        {
            ImGui::TextUnformatted( "Generating…" );
        }
        s_wasActive = true;
    }
    else
    {
        if( ImGui::IsWindowAppearing() || m_focusInput )
        {
            ImGui::SetKeyboardFocusHere( 0 );
            m_focusInput = false;
        }
        m_jobsLock.unlock();
        const char* buttonText = ICON_FA_PAPER_PLANE;
        auto buttonSize = ImGui::CalcTextSize( buttonText );
        buttonSize.x += ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
        ImGui::PushItemWidth( ImGui::GetContentRegionAvail().x - buttonSize.x );
        if( inputChanged ) ImGui::GetInputTextState( ImGui::GetCurrentWindow()->GetID( "##chat_input" ) )->ReloadUserBufAndMoveToEnd();
        bool send;
        if( m_suggestion.empty() )
        {
            send = ImGui::InputTextWithHint( "##chat_input", "Write your question here…", m_input, InputBufferSize, ImGuiInputTextFlags_EnterReturnsTrue );
        }
        else
        {
            std::string hint = ICON_FA_COMMENT_DOTS " " + m_suggestion;
            send = ImGui::InputTextWithHint( "##chat_input", hint.c_str(), m_input, InputBufferSize, ImGuiInputTextFlags_EnterReturnsTrue );
        }
        ImGui::SameLine();
        const bool inputEmpty = *m_input == 0;
        const bool hasSuggestion = !m_suggestion.empty();
        if( inputEmpty && !hasSuggestion ) ImGui::BeginDisabled();
        send |= ImGui::Button( buttonText );
        if( inputEmpty && !hasSuggestion ) ImGui::EndDisabled();
        if( send )
        {
            if( inputEmpty && hasSuggestion )
            {
                const auto sz = std::min( InputBufferSize - 1, m_suggestion.size() );
                memcpy( m_input, m_suggestion.data(), sz );
                m_input[sz] = 0;
            }
            auto ptr = m_input;
            while( *ptr )
            {
                if( *ptr != ' ' && *ptr != '\t' && *ptr != '\n' ) break;
                ptr++;
            }
            if( *ptr )
            {
                std::lock_guard lock( m_jobsLock );
                AddMessage( ptr, "user" );
                *m_input = 0;
                m_suggestion.clear();
                QueueSendMessage();
            }
            else
            {
                *m_input = 0;
            }
            ImGui::SetKeyboardFocusHere( -1 );
        }
    }

    ImGui::End();
}

void TracyLlm::WorkerThread()
{
    std::unique_lock lock( m_jobsLock );
    while( !m_exit.load( std::memory_order_acquire ) )
    {
        m_cv.wait( lock, [this] { return !m_jobs.empty() || m_exit.load( std::memory_order_acquire ); } );
        if( m_exit.load( std::memory_order_acquire ) ) break;

        m_currentJob = std::move( m_jobs.front() );
        m_jobs.erase( m_jobs.begin() );

        switch( m_currentJob->task )
        {
        case Task::Connect:
        {
            auto callback = m_currentJob->callback;
            m_busy = true;
            lock.unlock();
            m_api->Connect( s_config.llmAddress.c_str() );
            callback();
            lock.lock();
            m_busy = false;
            break;
        }
        case Task::SendMessage:
            lock.unlock();
            SendMessage();
            lock.lock();
            break;
        case Task::FastMessage:
        {
            auto param = m_currentJob->param2;
            auto callback = m_currentJob->callback2;
            lock.unlock();
            const int modelIdx = s_config.llmSeparateFastModel ? m_fastIdx : m_modelIdx;
            auto response = m_api->SendMessage( param, modelIdx );
            callback( response );
            lock.lock();
            break;
        }
        case Task::Tokenize:
        {
            auto param = m_currentJob->param;
            auto callback = m_currentJob->callback2;
            lock.unlock();
            auto tokens = m_api->Tokenize( param, m_modelIdx );
            if( tokens < 0 ) tokens = param.size() / 4;
            callback( { { "tokens", tokens } } );
            lock.lock();
            break;
        }
        }

        m_currentJob.reset();
    }
};

void TracyLlm::UpdateModels()
{
    m_modelIdx = -1;
    m_fastIdx = -1;
    m_embedIdx = -1;

    auto& models = m_api->GetModels();
    auto it = std::ranges::find_if( models, []( const auto& model ) { return model.name == s_config.llmModel; } );
    if( it == models.end() )
    {
        for( int i=0; i<models.size(); i++ )
        {
            if( !models[i].embeddings )
            {
                m_modelIdx = i;
                break;
            }
        }
    }
    else
    {
        m_modelIdx = std::distance( models.begin(), it );
    }

    it = std::ranges::find_if( models, []( const auto& model ) { return model.name == s_config.llmFastModel; } );
    if( it == models.end() )
    {
        for( int i=0; i<models.size(); i++ )
        {
            if( !models[i].embeddings )
            {
                m_fastIdx = i;
                break;
            }
        }
    }
    else
    {
        m_fastIdx = std::distance( models.begin(), it );
    }

    it = std::ranges::find_if( models, []( const auto& model ) { return model.name == s_config.llmEmbeddingsModel; } );
    if( it == models.end() )
    {
        for( int i=0; i<models.size(); i++ )
        {
            if( models[i].embeddings )
            {
                m_embedIdx = i;
                break;
            }
        }
    }
    else
    {
        m_embedIdx = std::distance( models.begin(), it );
    }

    if( m_embedIdx >= 0 )
    {
        m_tools->SelectManualEmbeddings( models[m_embedIdx].name );
    }
}

static void Replace( std::string& str, std::string_view from, std::string_view to )
{
    std::string::size_type pos;
    while( ( pos = str.find( from ) ) != std::string::npos )
    {
        str.replace( pos, from.size(), to );
    }
}

void TracyLlm::ResetChat()
{
    *m_input = 0;
    m_usedCtx = 0;
    m_chat.clear();
    m_summary.clear();
    m_suggestion.clear();

    UpdateSystemPrompt();

    m_chatId.fetch_add( 1, std::memory_order_release );
}

void TracyLlm::UpdateSystemPrompt()
{
    static constexpr std::string_view UserToken = "%USER%";
    static constexpr std::string_view TimeToken = "%TIME%";
    static constexpr std::string_view ProgramNameToken = "%PROGRAMNAME%";
    static constexpr std::string_view ProgramTimeToken = "%PROGRAMTIME%";
    static constexpr std::string_view ProfileTimeToken = "%PROFILETIME%";
    static constexpr std::string_view ProfileLengthToken = "%PROFILELENGTH%";
    static constexpr std::string_view ProfileDescriptionToken = "%PROFILEDESCRIPTION%";
    static constexpr std::string_view SkillsToken = "%SKILLS%";
    static constexpr std::string_view PersonalityToken = "%PERSONALITY%";

    auto userName = GetUserFullName();
    if( !userName ) userName = GetUserLogin();

    const auto exectime = m_worker.GetExecutableTime();
    const auto capturetime = m_worker.GetCaptureTime();
    const auto firstTime = m_worker.GetFirstTime();
    const auto lastTime = m_worker.GetLastTime();

    char etime[64], ctime[64];

    time_t et = exectime;
    auto elt = localtime( &et );
    if( elt ) strftime( etime, 64, "%F %T", elt );
    else strcpy( etime, "unknown" );

    time_t ct = capturetime;
    auto clt = localtime( &ct );
    if( clt ) strftime( ctime, 64, "%F %T", clt );
    else strcpy( ctime, "unknown" );

    std::string descStr;
    const auto& desc = m_view.GetUserData().GetDescription();
    if( !desc.empty() ) descStr += " Profiling session description: '" + desc + "'.";
    const auto& filename = m_view.GetFilename();
    if( !filename.empty() ) descStr += " Profiling session file: '" + filename + "'.";

    std::string skills;
    for( auto& skill : m_skills ) skills += skill.name + ": " + skill.description + "\n";

    auto systemPrompt = std::string( m_systemPrompt->data(), m_systemPrompt->size() );
    m_personalityPrompt = std::clamp<size_t>( s_config.llmPersonality, 0, m_personality.size() - 1 );

    Replace( systemPrompt, UserToken, userName );
    Replace( systemPrompt, TimeToken, m_tools->GetCurrentTime() );
    Replace( systemPrompt, ProgramNameToken, m_worker.GetCaptureProgram() );
    Replace( systemPrompt, ProgramTimeToken, etime );
    Replace( systemPrompt, ProfileTimeToken, ctime );
    Replace( systemPrompt, ProfileLengthToken, TimeToString( lastTime - firstTime ) );
    Replace( systemPrompt, ProfileDescriptionToken, descStr );
    Replace( systemPrompt, SkillsToken, skills );
    Replace( systemPrompt, PersonalityToken, m_personality[m_personalityPrompt] );

    if( !m_api || m_chatId.load( std::memory_order_acquire ) == 0 )
    {
        m_chat.push_back( {
            { "role", "system" },
            { "content", systemPrompt }
        } );
    }
    else if( m_chat.empty() )
    {
        std::lock_guard lock( m_jobsLock );
        AddMessage( std::move( systemPrompt ), "system" );
    }
    else
    {
        m_chat[0]["content"] = systemPrompt;
    }
}

// requires m_jobsLock
void TracyLlm::QueueConnect()
{
    m_jobs.emplace_back( std::make_shared<WorkItem>( WorkItem {
        .task = Task::Connect,
        .callback = [this] { UpdateModels(); }
    } ) );
    m_cv.notify_all();
}

bool TracyLlm::QueueSendMessageLocking()
{
    std::unique_lock<std::mutex> lock( m_jobsLock );
    return QueueSendMessage();
}

// requires m_jobsLock
bool TracyLlm::QueueSendMessage()
{
    if( !m_api->IsConnected() || m_modelIdx < 0 ) return false;
    m_jobs.emplace_back( std::make_shared<WorkItem>( WorkItem {
        .task = Task::SendMessage
    } ) );
    m_cv.notify_all();
    return true;
}

bool TracyLlm::QueueFastMessageLocking( const nlohmann::json& req, std::function<void(nlohmann::json)> callback )
{
    std::unique_lock<std::mutex> lock( m_jobsLock );
    return QueueFastMessage( req, std::move( callback ) );
}

// requires m_jobsLock
bool TracyLlm::QueueFastMessage( const nlohmann::json& req, std::function<void(nlohmann::json)> callback )
{
    const int modelIdx = s_config.llmSeparateFastModel ? m_fastIdx : m_modelIdx;
    if( !m_api->IsConnected() || modelIdx < 0 ) return false;
    m_jobs.emplace_back( std::make_shared<WorkItem>( WorkItem {
        .task = Task::FastMessage,
        .callback2 = std::move( callback ),
        .param2 = req
    } ) );
    m_cv.notify_all();
    return true;
}

void TracyLlm::AddMessageLocking( std::string&& str, const char* role )
{
    std::unique_lock<std::mutex> lock( m_jobsLock );
    AddMessage( std::move( str ), role );
}

// requires m_jobsLock
void TracyLlm::AddMessage( std::string&& str, const char* role )
{
    assert( m_api );
    m_jobs.emplace_back( std::make_shared<WorkItem>( WorkItem {
        .task = Task::Tokenize,
        .callback2 = [this, str, role]( nlohmann::json json ) {
            std::lock_guard lock( m_chatLock );
            m_usedCtx += json["tokens"].get<int>();
            if( m_chat.size() == 1 ) UpdateSystemPrompt();
            nlohmann::json msg = {
                { "role", role },
                { "content", str }
            };
            m_chat.emplace_back( std::move( msg ) );
        },
        .param = std::move( str ),
    } ) );
    m_cv.notify_all();
}

void TracyLlm::AddMessageBlocking( std::string&& str, const char* role )
{
    assert( m_api );
    const auto tokens = m_api->Tokenize( str, m_modelIdx );
    m_usedCtx += tokens >= 0 ? tokens : str.size() / 4;

    nlohmann::json msg;
    msg["role"] = role;
    msg["content"] = std::move( str );

    std::lock_guard lock( m_chatLock );
    m_chat.emplace_back( std::move( msg ) );
}

void TracyLlm::AddMessageBlocking( nlohmann::json&& json )
{
    auto dump = json.dump();
    assert( m_api );
    const auto tokens = m_api->Tokenize( dump, m_modelIdx );
    m_usedCtx += tokens >= 0 ? tokens : dump.size() / 4;

    std::lock_guard lock( m_chatLock );
    m_chat.emplace_back( std::move( json ) );
}

void TracyLlm::AddAttachmentLocking( std::string&& str, const char* role )
{
    std::unique_lock<std::mutex> lock( m_jobsLock );
    AddMessage( "<attachment>\n" + std::move( str ), role );
}

void TracyLlm::ManageContext()
{
    const auto& models = m_api->GetModels();
    const auto ctxSize = models[m_modelIdx].contextSize;
    if( ctxSize <= 0 ) return;

    const auto quota = std::max( 4096, int( ctxSize * 0.8f ) );
    if( m_usedCtx < quota ) return;

    size_t idx = 0;
    std::vector<std::pair<size_t, size_t>> toolOutputs;
    for( auto& msg : m_chat )
    {
        if( msg["role"].get_ref<const std::string&>() == "tool" )
        {
            auto& str = msg["content"].get_ref<const std::string&>();
            toolOutputs.emplace_back( str.size(), idx );
        }
        idx++;
    }
    if( toolOutputs.size() > 1 )
    {
        // keep the last tool output
        toolOutputs.pop_back();

        // exponentially increase sizes of old tool outputs to prefer the most recent
        constexpr float K = 1.1f;
        for( size_t i=0; i<toolOutputs.size(); i++ ) toolOutputs[i].first *= std::pow( K, toolOutputs.size() - i );

        // remove the largest tool output
        std::ranges::stable_sort( toolOutputs, []( const auto& a, const auto& b ) { return a.first > b.first; } );
        for( auto& v : toolOutputs )
        {
            auto tokens = m_api->Tokenize( m_chat[v.second]["content"].get_ref<const std::string&>(), m_modelIdx );
            m_usedCtx -= tokens >= 0 ? tokens : v.first / 4;

            m_chat[v.second]["content"] = TracyLlmChat::ForgetMsg;
            tokens = m_api->Tokenize( TracyLlmChat::ForgetMsg, m_modelIdx );
            m_usedCtx += tokens >= 0 ? tokens : strlen( TracyLlmChat::ForgetMsg ) / 4;

            if( m_usedCtx < quota ) break;
        }
    }
}

void TracyLlm::SendMessage()
{
    std::unique_lock lock( m_chatLock );
    ManageContext();
    auto chat = m_chat;
    bool needSummary = m_summary.empty() && s_config.llmSummary;
    lock.unlock();

    for( auto& msg : chat )
    {
        if( msg.contains( "time_start" ) ) msg.erase( "time_start" );
        if( msg.contains( "time_end" ) ) msg.erase( "time_end" );
        if( msg.contains( "model" ) ) msg.erase( "model" );
    }

    if( needSummary && chat.size() == 2 )
    {
        auto& content = chat[1]["content"].get_ref<std::string&>();
        if( content.size() <= 30 )
        {
            needSummary = false;
            std::lock_guard lock( m_chatLock );
            m_summary = content;
        }
    }

    if( needSummary )
    {
        auto query = chat;
        query[0]["content"] = "Provide a one-line topic summary for the user input. Do NOT answer the question. The summary should be slogan-like, 5-8 words max. Reply with ONLY the summary, nothing else. Match the language of the user's query.";
        const int chatId = m_chatId.load( std::memory_order_acquire );
        QueueFastMessageLocking( query, [this, chatId]( const nlohmann::json& res ) {
            if( m_chatId.load( std::memory_order_acquire ) != chatId ) return;
            if( res.contains( "choices" ) )
            {
                auto& choices = res["choices"];
                if( choices.is_array() && !choices.empty() )
                {
                    auto& c0 = choices[0];
                    if( c0.contains( "message" ) )
                    {
                        auto& msg = c0["message"];
                        if( msg.contains( "role" ) && msg.contains( "content" ) )
                        {
                            auto& role = msg["role"];
                            auto& content = msg["content"];
                            if( role.is_string() && content.is_string() && msg["role"].get_ref<const std::string&>() == "assistant" )
                            {
                                auto& str = msg["content"].get_ref<const std::string&>();
                                if( str.size() <= 120 )
                                {
                                    if( str.find( '\n' ) != std::string::npos || str.find( '\r' ) != std::string::npos )
                                    {
                                        auto tmp = str;
                                        std::ranges::replace( tmp, '\n', ' ' );
                                        std::ranges::replace( tmp, '\r', ' ' );

                                        std::lock_guard lock( m_chatLock );
                                        m_summary = tmp;
                                    }
                                    else
                                    {
                                        std::lock_guard lock( m_chatLock );
                                        m_summary = str;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } );
    }

    try
    {
        AddMessageBlocking( {
            { "role", "assistant" },
            { "time_start", std::chrono::duration_cast<std::chrono::nanoseconds>( std::chrono::steady_clock::now().time_since_epoch() ).count() },
            { "model", m_api->GetModels()[m_modelIdx].name },
        } );

        size_t i = 1;
        while( i < chat.size() )
        {
            if( chat[i]["role"].get_ref<const std::string&>() == "user" &&
                chat[i-1]["role"].get_ref<const std::string&>() == "user" )
            {
                auto& str = chat[i-1]["content"].get_ref<std::string&>();
                assert( str.starts_with( "<attachment>\n" ) );
                str.append( "</attachment>\n\n" );
                str.append( chat[i]["content"].get_ref<const std::string&>() );
                chat.erase( chat.begin() + i );
            }
            else
            {
                i++;
            }
        }

        nlohmann::json req = {
            { "model", m_api->GetModels()[m_modelIdx].name },
            { "messages", std::move( chat ) },
            { "stream", true },
            { "cache_prompt", true },
            { "tools", m_toolsJson },
            { "chat_template_kwargs", {
                { "enable_thinking", true }
            } }
        };
        if( m_setTemperature ) req["temperature"] = m_temperature;

        m_api->ChatCompletion( req, [this]( const nlohmann::json& response ) -> bool { return OnResponse( response ); }, m_modelIdx );
    }
    catch( std::exception& e )
    {
        lock.lock();
        if( !m_chat.empty() && m_chat.back()["role"].get_ref<const std::string&>() == "assistant" ) m_chat.pop_back();
        lock.unlock();
        AddMessageBlocking( e.what(), "error" );
    }
}

void TracyLlm::AppendResponse( const char* name, const nlohmann::json& delta )
{
    if( delta.contains( name ) )
    {
        auto& json = delta[name];
        if( json.is_string() )
        {
            std::string str = json.get_ref<const std::string&>();
            std::erase( str, '\r' );

            auto& back = m_chat.back();
            if( back.contains( name ) )
            {
                assert( back[name].is_string() );
                back[name].get_ref<std::string&>().append( str );
            }
            else
            {
                back[name] = std::move( str );
            }

            m_usedCtx++;
        }
        else if( json.is_array() )
        {
            assert( json.size() == 1 );
            auto& val = json[0];
            auto index = val["index"].get<size_t>();

            auto& back = m_chat.back();
            if( !back.contains( name ) ) back[name] = nlohmann::json::array();

            auto& arr = back[name].get_ref<nlohmann::json::array_t&>();
            if( index == arr.size() )
            {
                arr.push_back( val );
            }
            else
            {
                arr[index]["function"]["arguments"].get_ref<std::string&>().append( val["function"]["arguments"].get_ref<const std::string&>() );
            }
        }
    }
}

bool TracyLlm::OnResponse( const nlohmann::json& json )
{
    std::unique_lock chatLock( m_chatLock );
    std::unique_lock jobsLock( m_jobsLock );
    if( m_currentJob->stop )
    {
        m_focusInput = true;
        return false;
    }
    jobsLock.unlock();

    assert( m_chat.back()["role"].get_ref<const std::string&>() == "assistant" );

    bool done = false;
    try
    {
        if( json.contains( "choices" ) )
        {
            auto& choices = json["choices"];
            if( !choices.empty() )
            {
                auto& node = choices[0];
                auto& delta = node["delta"];

                AppendResponse( "content", delta );
                AppendResponse( "reasoning_content", delta );
                AppendResponse( "tool_calls", delta );

                m_chat.back()["time_end"] = std::chrono::duration_cast<std::chrono::nanoseconds>( std::chrono::steady_clock::now().time_since_epoch() ).count();

                done = node.contains( "finish_reason" ) && !node["finish_reason"].empty();
            }
        }
        else if( json.contains( "error" ) )
        {
            jobsLock.lock();
            AddMessage( json["error"].dump( 2 ), "error" );
            m_focusInput = true;
            return false;
        }
    }
    catch( const nlohmann::json::exception& e )
    {
        jobsLock.lock();
        m_focusInput = true;
        return false;
    }

    if( done )
    {
        if( json.contains( "usage" ) )
        {
            auto& usage = json["usage"];
            if( usage.contains( "total_tokens" ) ) m_usedCtx = usage["total_tokens"].get<int>();
        }

        auto& back = m_chat.back();
        if( back.contains( "tool_calls" ) )
        {
            auto calls = back["tool_calls"];
            chatLock.unlock();
            for( auto& call : calls )
            {
                auto& id = call["id"].get_ref<const std::string&>();
                auto& function = call["function"];
                auto& name = function["name"].get_ref<const std::string&>();
                auto& arguments = function["arguments"].get_ref<const std::string&>();

                std::string result;
                try
                {
                    result = m_tools->HandleToolCalls( name, nlohmann::json::parse( arguments ), *m_api, m_api->GetModels()[m_modelIdx].contextSize, m_embedIdx >= 0 );
                }
                catch( const nlohmann::json::exception& e )
                {
                    result = nlohmann::json { "error", e.what() };
                }

                nlohmann::json reply = {
                    { "role", "tool" },
                    { "tool_call_id", id },
                    { "name", name },
                    { "content", result }
                };
                AddMessageBlocking( std::move( reply ) );
            }
            jobsLock.lock();
            QueueSendMessage();
        }
        else
        {
            auto chat = m_chat;
            chatLock.unlock();

            for( auto& msg : chat )
            {
                if( msg.contains( "time_start" ) ) msg.erase( "time_start" );
                if( msg.contains( "time_end" ) ) msg.erase( "time_end" );
                if( msg.contains( "model" ) ) msg.erase( "model" );
            }

            if( s_config.llmSuggestion )
            {
                auto suggestionQuery = chat;
                suggestionQuery.push_back( nlohmann::json {
                    {"role", "user"},
                    {"content", "Based on this conversation, suggest one useful follow-up question the user might want to ask next. It should be relevant, actionable, and something the user would genuinely want to explore. Reply with ONLY the question text, in the user's language, under 80 characters."}
                } );
                const int chatId = m_chatId.load( std::memory_order_acquire );
                QueueFastMessageLocking( suggestionQuery, [this, chatId]( const nlohmann::json& res ) {
                    if( m_chatId.load( std::memory_order_acquire ) != chatId ) return;
                    if( res.contains( "choices" ) )
                    {
                        auto& choices = res["choices"];
                        if( choices.is_array() && !choices.empty() )
                        {
                            if( choices[0].contains( "message" ) && choices[0]["message"].contains( "content" ) )
                            {
                                auto str = choices[0]["message"]["content"].get<std::string>();
                                std::ranges::replace( str, '\n', ' ' );
                                std::ranges::replace( str, '\r', ' ' );
                                std::lock_guard lock( m_chatLock );
                                m_suggestion = std::move( str );
                            }
                        }
                    }
                } );
            }

            jobsLock.lock();
            m_focusInput = true;
        }
    }

    return true;
}

void TracyLlm::SyncIntegratorFieldsFromConfig()
{
    auto clampCopy = []( char* dst, size_t dstSize, const std::string& src ) {
        const size_t n = src.size() < dstSize - 1 ? src.size() : dstSize - 1;
        memcpy( dst, src.c_str(), n );
        dst[n] = '\0';
    };
    clampCopy( m_integratorNode, sizeof( m_integratorNode ), s_config.llmIntegratorNode );
    clampCopy( m_integratorScript, sizeof( m_integratorScript ), s_config.llmIntegratorScript );
    clampCopy( m_integratorTracyRoot, sizeof( m_integratorTracyRoot ), s_config.llmIntegratorTracyRoot );
    clampCopy( m_integratorBridge, sizeof( m_integratorBridge ), s_config.llmIntegratorBridge );
}

void TracyLlm::StopIntegratorMcpProcess()
{
#if defined( TRACY_MCP_EMBEDDED )
    tracy::mcp::StopStreamableHttpInBackground();
    m_integratorMcpEmbedded = false;
#endif
#ifdef _WIN32
    if( m_integratorProcessHandle )
    {
        HANDLE h = (HANDLE)(uintptr_t)m_integratorProcessHandle;
        TerminateProcess( h, 1 );
        CloseHandle( h );
        m_integratorProcessHandle = 0;
    }
#else
    if( m_integratorChildPid > 0 )
    {
        kill( m_integratorChildPid, SIGTERM );
        int st = 0;
        waitpid( m_integratorChildPid, &st, 0 );
        m_integratorChildPid = -1;
    }
#endif
    m_integratorMcpRunning = false;
    m_integratorScriptUsed[0] = '\0';
    snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "Stopped." );
}

bool TracyLlm::StartIntegratorMcpProcess()
{
    if( m_integratorMcpRunning ) return true;

    std::string tracyRoot = s_config.llmIntegratorTracyRoot;
    while( !tracyRoot.empty() && ( tracyRoot.back() == '/' || tracyRoot.back() == '\\' ) ) tracyRoot.pop_back();
    {
        if( tracyRoot.empty() )
        {
            const std::string ex = GetExecutablePath();
            if( !ex.empty() )
            {
                std::string walk = ParentPath( ex );
                for( int k = 0; k < 16 && !walk.empty(); ++k )
                {
                    if( FileExists( JoinUnder( walk, "manual/tracy.md" ).c_str() ) || FileExists( JoinUnder( walk, "manual\\tracy.md" ).c_str() ) )
                    {
                        tracyRoot = walk;
                        break;
                    }
                    const std::string n = ParentPath( walk );
                    if( n == walk ) break;
                    walk = n;
                }
            }
        }
    }
    while( !tracyRoot.empty() && ( tracyRoot.back() == '/' || tracyRoot.back() == '\\' ) ) tracyRoot.pop_back();

    const std::string portStr = std::to_string( s_config.llmIntegratorMcpPort );

#if defined( TRACY_MCP_EMBEDDED )
    if( s_config.llmIntegratorScript.empty() )
    {
#ifdef _WIN32
        _putenv_s( "TRACY_MCP_HTTP_PORT", portStr.c_str() );
        SetEnvironmentVariableA( "TRACY_MCP_BRIDGE", NULL );
        if( !tracyRoot.empty() ) _putenv_s( "TRACY_ROOT", tracyRoot.c_str() );
#else
        if( setenv( "TRACY_MCP_HTTP_PORT", portStr.c_str(), 1 ) != 0 )
        {
            snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "setenv TRACY_MCP_HTTP_PORT failed." );
            return false;
        }
        if( !tracyRoot.empty() && setenv( "TRACY_ROOT", tracyRoot.c_str(), 1 ) != 0 )
        {
            snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "setenv TRACY_ROOT failed." );
            return false;
        }
        unsetenv( "TRACY_MCP_BRIDGE" );
#endif
        if( tracy::mcp::StartStreamableHttpInBackground( s_config.llmIntegratorMcpPort, "127.0.0.1" ) )
        {
            m_integratorMcpEmbedded = true;
            m_integratorMcpRunning = true;
            const std::string exel = GetExecutablePath();
            const char* tag = exel.empty() ? "tracy.exe (embedded MCP)" : exel.c_str();
            snprintf( m_integratorScriptUsed, sizeof( m_integratorScriptUsed ), "%s", tag );
            snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "Embedded native MCP — http://127.0.0.1:%d/mcp",
                s_config.llmIntegratorMcpPort );
            return true;
        }
        if( tracy::mcp::IsStreamableHttpMcpRunning() )
        {
            m_integratorMcpEmbedded = true;
            m_integratorMcpRunning = true;
            const std::string exel = GetExecutablePath();
            const char* tag = exel.empty() ? "tracy.exe (embedded MCP)" : exel.c_str();
            snprintf( m_integratorScriptUsed, sizeof( m_integratorScriptUsed ), "%s", tag );
            snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "Embedded native MCP — http://127.0.0.1:%d/mcp",
                s_config.llmIntegratorMcpPort );
            return true;
        }
        snprintf( m_integratorStatus, sizeof( m_integratorStatus ),
            "Embedded MCP could not listen (port %d in use?). Trying external tracy-mcp-bridge or Node…", s_config.llmIntegratorMcpPort );
    }
#endif

#if defined( TRACY_MCP_EMBEDDED )
    if( s_config.llmIntegratorScript.empty() && tracy::mcp::IsStreamableHttpMcpRunning() )
    {
        m_integratorMcpEmbedded = true;
        m_integratorMcpRunning = true;
        const std::string exel = GetExecutablePath();
        const char* tag = exel.empty() ? "tracy.exe (embedded MCP)" : exel.c_str();
        snprintf( m_integratorScriptUsed, sizeof( m_integratorScriptUsed ), "%s", tag );
        snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "Embedded native MCP — http://127.0.0.1:%d/mcp",
            s_config.llmIntegratorMcpPort );
        return true;
    }
#endif

    std::string nativeBridge;
    const bool tryNative = s_config.llmIntegratorScript.empty() && FindNativeMcpBridgeExecutable( nativeBridge );

    if( tryNative )
    {
#ifdef _WIN32
        _putenv_s( "TRACY_MCP_HTTP_PORT", portStr.c_str() );
        SetEnvironmentVariableA( "TRACY_MCP_BRIDGE", NULL );
        if( !tracyRoot.empty() ) _putenv_s( "TRACY_ROOT", tracyRoot.c_str() );
        {
            const size_t n = nativeBridge.size() < sizeof( m_integratorScriptUsed ) - 1 ? nativeBridge.size() : sizeof( m_integratorScriptUsed ) - 1;
            memcpy( m_integratorScriptUsed, nativeBridge.c_str(), n );
            m_integratorScriptUsed[n] = '\0';
        }

        std::vector<char> cmdBuf( nativeBridge.begin(), nativeBridge.end() );
        cmdBuf.push_back( '\0' );

        STARTUPINFOA siN = {};
        siN.cb = sizeof( siN );
        PROCESS_INFORMATION piN = {};
        const char* cwdN = tracyRoot.empty() ? NULL : tracyRoot.c_str();
        if( !CreateProcessA( nativeBridge.c_str(), cmdBuf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, cwdN, &siN, &piN ) )
        {
            const DWORD err = GetLastError();
            snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "CreateProcess (native tracy-mcp-bridge) failed (error %lu).", (unsigned long)err );
            m_integratorScriptUsed[0] = '\0';
            return false;
        }
        CloseHandle( piN.hThread );
        m_integratorProcessHandle = (uint64_t)(uintptr_t)piN.hProcess;
        m_integratorMcpRunning = true;
        snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "Native MCP — PID %lu — http://127.0.0.1:%d/mcp", (unsigned long)piN.dwProcessId, s_config.llmIntegratorMcpPort );
        return true;
#else
        if( setenv( "TRACY_MCP_HTTP_PORT", portStr.c_str(), 1 ) != 0 )
        {
            snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "setenv TRACY_MCP_HTTP_PORT failed." );
            return false;
        }
        if( !tracyRoot.empty() && setenv( "TRACY_ROOT", tracyRoot.c_str(), 1 ) != 0 )
        {
            snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "setenv TRACY_ROOT failed." );
            return false;
        }
        unsetenv( "TRACY_MCP_BRIDGE" );
        {
            const size_t n = nativeBridge.size() < sizeof( m_integratorScriptUsed ) - 1 ? nativeBridge.size() : sizeof( m_integratorScriptUsed ) - 1;
            memcpy( m_integratorScriptUsed, nativeBridge.c_str(), n );
            m_integratorScriptUsed[n] = '\0';
        }
        const pid_t pidN = fork();
        if( pidN < 0 )
        {
            snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "fork() failed." );
            m_integratorScriptUsed[0] = '\0';
            return false;
        }
        if( pidN == 0 )
        {
            if( !tracyRoot.empty() ) chdir( tracyRoot.c_str() );
            const char* av[] = { nativeBridge.c_str(), nullptr };
            execv( nativeBridge.c_str(), (char* const*)av );
            _exit( 127 );
        }
        m_integratorChildPid = pidN;
        m_integratorMcpRunning = true;
        snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "Native MCP — PID %d — http://127.0.0.1:%d/mcp", (int)pidN, s_config.llmIntegratorMcpPort );
        return true;
#endif
    }

    std::string script;
    std::string repoResolved;
    char resolveErr[512];
    if( !ResolveIntegratorPaths( script, repoResolved, resolveErr, sizeof( resolveErr ) ) )
    {
        snprintf( m_integratorStatus, sizeof( m_integratorStatus ),
            "%s\n\nWith an empty integrator script, Tracy starts native MCP in-process. If that fails, it looks for a separate tracy-mcp-bridge next to the executable or in a dev mcp/bridge build folder, then Node.",
            resolveErr );
        m_integratorScriptUsed[0] = '\0';
        return false;
    }

    {
        const size_t n = script.size() < sizeof( m_integratorScriptUsed ) - 1 ? script.size() : sizeof( m_integratorScriptUsed ) - 1;
        memcpy( m_integratorScriptUsed, script.c_str(), n );
        m_integratorScriptUsed[n] = '\0';
    }

    if( tracyRoot.empty() ) tracyRoot = repoResolved;
    while( !tracyRoot.empty() && ( tracyRoot.back() == '/' || tracyRoot.back() == '\\' ) ) tracyRoot.pop_back();

#ifdef _WIN32
    char nodeExe[MAX_PATH];
    const std::string& nodeCfg = s_config.llmIntegratorNode;
    if( nodeCfg.empty() || nodeCfg == "node" )
    {
        if( !SearchPathA( NULL, "node.exe", NULL, (DWORD)sizeof( nodeExe ), nodeExe, NULL ) )
        {
            snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "node.exe not on PATH. Install Node.js, set \"Node executable\" in Advanced, or rely on in-process / external tracy-mcp-bridge (empty integrator script) for native MCP." );
            return false;
        }
    }
    else
    {
        strncpy( nodeExe, nodeCfg.c_str(), sizeof( nodeExe ) - 1 );
        nodeExe[sizeof( nodeExe ) - 1] = '\0';
    }

    _putenv_s( "TRACY_MCP_HTTP_PORT", portStr.c_str() );
    if( !tracyRoot.empty() ) _putenv_s( "TRACY_ROOT", tracyRoot.c_str() );
    if( !s_config.llmIntegratorBridge.empty() ) _putenv_s( "TRACY_MCP_BRIDGE", s_config.llmIntegratorBridge.c_str() );

    std::string cmdLine = std::string( "\"" ) + nodeExe + "\" \"" + script + "\"";
    std::vector<char> ncmd( cmdLine.begin(), cmdLine.end() );
    ncmd.push_back( '\0' );

    STARTUPINFOA si2 = {};
    si2.cb = sizeof( si2 );
    PROCESS_INFORMATION pi2 = {};

    const char* cwd2 = tracyRoot.empty() ? NULL : tracyRoot.c_str();
    if( !CreateProcessA( nodeExe, ncmd.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, cwd2, &si2, &pi2 ) )
    {
        const DWORD err = GetLastError();
        snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "CreateProcess failed (error %lu). Check Node and script paths.", (unsigned long)err );
        return false;
    }

    CloseHandle( pi2.hThread );
    m_integratorProcessHandle = (uint64_t)(uintptr_t)pi2.hProcess;
    m_integratorMcpRunning = true;
    snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "Node MCP — PID %lu — http://127.0.0.1:%d/mcp", (unsigned long)pi2.dwProcessId, s_config.llmIntegratorMcpPort );
    return true;
#else
    if( setenv( "TRACY_MCP_HTTP_PORT", portStr.c_str(), 1 ) != 0 )
    {
        snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "setenv TRACY_MCP_HTTP_PORT failed." );
        return false;
    }
    if( !tracyRoot.empty() )
    {
        if( setenv( "TRACY_ROOT", tracyRoot.c_str(), 1 ) != 0 )
        {
            snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "setenv TRACY_ROOT failed." );
            return false;
        }
    }
    if( !s_config.llmIntegratorBridge.empty() )
    {
        if( setenv( "TRACY_MCP_BRIDGE", s_config.llmIntegratorBridge.c_str(), 1 ) != 0 )
        {
            snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "setenv TRACY_MCP_BRIDGE failed." );
            return false;
        }
    }

    const std::string nodeArgStr = s_config.llmIntegratorNode.empty() ? std::string( "node" ) : s_config.llmIntegratorNode;
    const pid_t pid = fork();
    if( pid < 0 )
    {
        snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "fork() failed." );
        return false;
    }
    if( pid == 0 )
    {
        if( !tracyRoot.empty() ) chdir( tracyRoot.c_str() );
        execlp( nodeArgStr.c_str(), nodeArgStr.c_str(), script.c_str(), (char*)nullptr );
        _exit( 127 );
    }

    m_integratorChildPid = pid;
    m_integratorMcpRunning = true;
    snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "Node MCP — PID %d — http://127.0.0.1:%d/mcp", (int)pid, s_config.llmIntegratorMcpPort );
    return true;
#endif
}

void TracyLlm::UpdateIntegratorMcpProcess()
{
    if( !m_integratorDidBootstrap )
    {
        m_integratorDidBootstrap = true;
        if( s_config.llmIntegratorMcpEnabled ) m_integratorNeedStart = true;
    }

#if defined( TRACY_MCP_EMBEDDED )
    if( !m_integratorMcpEmbedded )
#endif
    {
#ifdef _WIN32
        if( m_integratorProcessHandle )
        {
            HANDLE h = (HANDLE)(uintptr_t)m_integratorProcessHandle;
            if( WaitForSingleObject( h, 0 ) == WAIT_OBJECT_0 )
            {
                DWORD code = 1;
                GetExitCodeProcess( h, &code );
                CloseHandle( h );
                m_integratorProcessHandle = 0;
                m_integratorMcpRunning = false;
                snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "Integrator exited (code %lu).", (unsigned long)code );
                if( s_config.llmIntegratorMcpEnabled ) m_integratorNeedStart = true;
            }
        }
#else
        if( m_integratorChildPid > 0 )
        {
            int st = 0;
            const pid_t r = waitpid( m_integratorChildPid, &st, WNOHANG );
            if( r == m_integratorChildPid )
            {
                m_integratorChildPid = -1;
                m_integratorMcpRunning = false;
                snprintf( m_integratorStatus, sizeof( m_integratorStatus ), "Integrator exited (status %d).", st );
                if( s_config.llmIntegratorMcpEnabled ) m_integratorNeedStart = true;
            }
            else if( r < 0 )
            {
                m_integratorChildPid = -1;
                m_integratorMcpRunning = false;
            }
        }
#endif
    }

    const bool want = s_config.llmIntegratorMcpEnabled;
    if( !want && m_integratorMcpRunning ) StopIntegratorMcpProcess();

    if( want && m_integratorNeedStart && !m_integratorMcpRunning )
    {
        m_integratorNeedStart = false;
        if( !StartIntegratorMcpProcess() ) m_integratorNeedStart = false;
    }
}

void TracyLlm::AddSkill( std::string&& name, std::string&& description, const std::shared_ptr<EmbedData>& content )
{
    m_skills.emplace_back( std::move( name ), std::move( description ), std::string( content->data(), content->size() ) );
}

void TracyLlm::AddPersonality( const std::shared_ptr<EmbedData>& content )
{
    m_personality.emplace_back( content->data(), content->size() );
}

}
