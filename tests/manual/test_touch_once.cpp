#include "vpet/animation_resource_manager.h"
#include "vpet/pet_state_machine.h"
#include "vpet/common_types.h"

#include <QCoreApplication>
#include <iostream>

static void PrintState(const vpet::PetStateMachine &machine)
{
    std::cout << "state=" << static_cast<int>(machine.GetCurrentState())
              << " segment=" << vpet::AnimationTypeToString(machine.IsInLoopSegment()
                     ? vpet::ANIMATION_TYPE::B_LOOP
                     : vpet::ANIMATION_TYPE::SINGLE).toStdString()
              << " frame=" << machine.GetCurrentFrame().GetImagePath().toStdString()
              << std::endl;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QString basePath = QStringLiteral("F:/Pet Agent/Animation");
    vpet::AnimationResourceManager manager(basePath);

    if (!manager.LoadAll())
    {
        std::cout << "Failed to load animations" << std::endl;
        return 1;
    }

    vpet::PetStateMachine machine(manager);
    machine.Initialize();

    std::cout << "--- ClickHead ---" << std::endl;
    machine.ClickHead();
    PrintState(machine);

    for (int i = 0; i < 300; ++i)
    {
        machine.Update(50);

        if ((i % 20) == 0)
        {
            PrintState(machine);
        }
    }

    std::cout << "--- Final ---" << std::endl;
    PrintState(machine);

    return 0;
}
