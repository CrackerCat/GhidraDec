/*
#include <nc/config.h>
#include <nc/common/Branding.h>
#include <nc/common/make_unique.h>
#include <nc/common/Types.h>
#include <nc/core/image/ByteSource.h>
#include <nc/core/Context.h>
#include <nc/core/arch/Instruction.h>
#include <nc/core/arch/Architecture.h>
#include <nc/core/image/Image.h>
#include <nc/core/mangling/DefaultDemangler.h>
#include <nc/core/image/Section.h>
#include <nc/core/image/Relocation.h>
#include <nc/gui/MainWindow.h>
#include <nc/gui/Project.h>
#include <nc/gui/InstructionsView.h>
#include <nc/gui/CxxView.h>
*/

#include "GhidraDecView.h"
#include <QVBoxLayout>
#include <QPlainTextEdit>
#include <QTreeView>
#include <QApplication>
#include <_scriptapi_module.h>
#include <_scriptapi_memory.h>
#include <_scriptapi_symbol.h>

using namespace Script;

class DbgByteSource : public nc::core::image::ByteSource
{
public:
    explicit DbgByteSource(nc::ByteAddr lowerBound, nc::ByteAddr upperBound)
        : mLowerBound(lowerBound),
        mUpperBound(upperBound)
    {
    }

    nc::ByteSize readBytes(nc::ByteAddr addr, void* buf, nc::ByteSize size) const override
    {
        if (addr < mLowerBound || addr + size >= mUpperBound)
            return 0;
        duint sizeRead = 0;
        Memory::Read(addr, buf, size, &sizeRead);
        return sizeRead;
    }

private:
    nc::ByteAddr mLowerBound;
    nc::ByteAddr mUpperBound;
};

static std::unique_ptr<nc::gui::Project> MakeProject(duint base, duint size)
{
    using namespace Script;

    auto project = std::make_unique<nc::gui::Project>();
    auto image = project->image().get();

    //set architecture
#ifdef _WIN64
    image->platform().setArchitecture(QLatin1String("x86-64"));
#else //x86
    image->platform().setArchitecture(QLatin1String("i386"));
#endif //_WIN64

    //set OS
    image->platform().setOperatingSystem(nc::core::image::Platform::Windows);

    //set demangler
    image->setDemangler(std::make_unique<nc::core::mangling::DefaultDemangler>());

    //create sections
    auto success = false;
    auto moduleBase = Module::BaseFromAddr(base);
    if (moduleBase)
    {
        BridgeList<Module::ModuleSectionInfo> sections;
        if (Module::SectionListFromAddr(moduleBase, &sections))
        {
            success = true;
            for (auto i = 0; i < sections.Count(); i++)
            {
                auto sectionAddr = sections[i].addr;
                auto sectionSize = sections[i].size;
                auto section = std::make_unique<nc::core::image::Section>(sections[i].name, sectionAddr, sectionSize);
                section->setReadable(true);
                section->setWritable(true);
                section->setExecutable(true);
                section->setCode(true);
                section->setData(true);
                section->setAllocated(true);
                section->setExternalByteSource(std::make_unique<DbgByteSource>(sectionAddr, sectionAddr + sectionSize));
                image->addSection(std::move(section));
            }
        }
    }

    //add a made-up section in case there was an error with the module sections.
    if (!success)
    {
        auto section = std::make_unique<nc::core::image::Section>(".text", base, size);
        section->setReadable(true);
        section->setWritable(true);
        section->setExecutable(true);
        section->setCode(true);
        section->setData(true);
        section->setAllocated(true);
        section->setExternalByteSource(std::make_unique<DbgByteSource>(base, base + size));
        image->addSection(std::move(section));
    }

    //add symbols
    BridgeList<Symbol::SymbolInfo> symbols;
    if (Symbol::GetList(&symbols))
    {
        for (auto i = 0; i < symbols.Count(); i++)
        {
            const auto& symbol = symbols[i];
            auto modBase = Module::BaseFromName(symbol.mod);
            if (!modBase)
                continue;
            auto va = modBase + symbol.rva;
            auto name = QString::fromUtf8(symbol.name);
            if (symbol.type == Symbol::Import) //IAT entry
                image->addRelocation(std::make_unique<nc::core::image::Relocation>(
                    va,
                    image->addSymbol(std::make_unique<nc::core::image::Symbol>(nc::core::image::SymbolType::FUNCTION, name, boost::none)),
                    image->platform().architecture()->bitness() / CHAR_BIT));
            else //Function or Export
                image->addSymbol(std::make_unique<nc::core::image::Symbol>(nc::core::image::SymbolType::FUNCTION, name, va));
        }
    }

    return project;
}

void GhidraDecView::decompileAt(const GhidraDecRange* ranges, duint count) const
{
    nc::gui::MainWindow* mainWindow = (nc::gui::MainWindow*)mGhidraDecMainWindow;
    duint base = Module::BaseFromAddr(ranges->start);
    duint size = Module::SizeFromAddr(base);
    if (!base || !size) //we are not inside a module
        base = DbgMemFindBaseAddr(ranges->start, &size);
    mainWindow->open(MakeProject(base, size));
    mainWindow->project()->setName("GhidraDec");
    for (duint i = 0; i < count; i++)
        mainWindow->project()->disassemble(mainWindow->project()->image().get(), ranges[i].start, ranges[i].end + 1);
    mainWindow->project()->decompile();
}

GhidraDecView::GhidraDecView(QWidget* parent) : QWidget(parent)
{
    nc::Branding branding = nc::branding();
    branding.setApplicationName("GhidraDec");
    branding.setOrganizationDomain("gmorsecode.com");
    branding.setOrganizationName("GMorseCode");
    nc::gui::MainWindow* mainWindow = new nc::gui::MainWindow(std::move(branding), parent);
    mainWindow->setAutoFillBackground(true);
    QVBoxLayout* layout = new QVBoxLayout();
    layout->addWidget(mainWindow);
    layout->setMargin(0);
    setLayout(layout);

    mGhidraDecMainWindow = mainWindow;

    //remove open/quit options
    mainWindow->openAction()->setEnabled(false);
    mainWindow->openAction()->setVisible(false);
    mainWindow->quitAction()->setEnabled(false);
    mainWindow->quitAction()->setVisible(false);

    //add command to jump from GhidraDec to x64dbg
    mJumpFromInstructionsViewAction = new QAction(tr("Show in x64dbg"), this);
    mJumpFromInstructionsViewAction->setShortcut(Qt::CTRL + Qt::Key_Backspace);
    mJumpFromInstructionsViewAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(mJumpFromInstructionsViewAction, SIGNAL(triggered()), this, SLOT(jumpFromInstructionsView()));
    mainWindow->instructionsView()->treeView()->addAction(mJumpFromInstructionsViewAction);

    mJumpFromCxxViewAction = new QAction(tr("Show in x64dbg"), this);
    mJumpFromCxxViewAction->setShortcut(Qt::CTRL + Qt::Key_Backspace);
    mJumpFromCxxViewAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(mJumpFromCxxViewAction, SIGNAL(triggered()), this, SLOT(jumpFromCxxView()));
    mainWindow->cxxView()->textEdit()->addAction(mJumpFromCxxViewAction);

    connect(mainWindow->instructionsView(), SIGNAL(contextMenuCreated(QMenu*)), this, SLOT(populateInstructionsContextMenu(QMenu*)));
    connect(mainWindow->cxxView(), SIGNAL(contextMenuCreated(QMenu*)), this, SLOT(populateCxxContextMenu(QMenu*)));

    qApp->setStyleSheet(R"(nc--gui--CxxView QPlainTextEdit {
    color: white;
    background-color: #272822;
}

nc--gui--CxxFormatting {
  qproperty-textColor: #FFFFFF;
  qproperty-singleLineCommentColor: #57A64A;
  qproperty-multiLineLineCommentColor: #57A64A;
  qproperty-keywordColor: #569CD6;
  qproperty-operatorColor: #B4B4B4;
  qproperty-numberColor: #B5CEA8;
  qproperty-macroColor: #BD63C5;
  qproperty-stringColor: #D69D85;
  qproperty-escapeCharColor: #4EC9B3;
}
)" + qApp->styleSheet());
}

void GhidraDecView::closeEvent(QCloseEvent* event)
{
    Q_UNUSED(event);
    mGhidraDecMainWindow->close();
}

void GhidraDecView::populateInstructionsContextMenu(QMenu* menu) const
{
    for (QAction* action : menu->actions())
    {
        if (action->shortcut() == QKeySequence(QKeySequence::FindNext))
            action->setShortcut(QKeySequence("Ctrl+F3"));
    }
    nc::gui::MainWindow* mainWindow = (nc::gui::MainWindow*)mGhidraDecMainWindow;
    if (!mainWindow->instructionsView()->selectedInstructions().empty())
    {
        menu->addSeparator();
        menu->addAction(mJumpFromInstructionsViewAction);
    }
}

void GhidraDecView::populateCxxContextMenu(QMenu* menu) const
{
    for (QAction* action : menu->actions())
    {
        if (action->shortcut() == QKeySequence(QKeySequence::FindNext))
            action->setShortcut(QKeySequence("Ctrl+F3"));
    }
    nc::gui::MainWindow* mainWindow = (nc::gui::MainWindow*)mGhidraDecMainWindow;
    if (!mainWindow->cxxView()->selectedInstructions().empty())
    {
        menu->addSeparator();
        menu->addAction(mJumpFromCxxViewAction);
    }
}

void GhidraDecView::jumpFromInstructionsView() const
{
    nc::gui::MainWindow* mainWindow = (nc::gui::MainWindow*)mGhidraDecMainWindow;
    for (auto instruction : mainWindow->instructionsView()->selectedInstructions())
    {
        duint addr = instruction->addr();
        DbgCmdExecDirect(QString().sprintf("disasm \"%p\"", addr).toUtf8().constData());
        GuiShowCpu();
        break;
    }
}

void GhidraDecView::jumpFromCxxView() const
{
    nc::gui::MainWindow* mainWindow = (nc::gui::MainWindow*)mGhidraDecMainWindow;
    for (auto instruction : mainWindow->cxxView()->selectedInstructions())
    {
        duint addr = instruction->addr();
        DbgCmdExecDirect(QString().sprintf("disasm \"%p\"", addr).toUtf8().constData());
        GuiShowCpu();
        break;
    }
}