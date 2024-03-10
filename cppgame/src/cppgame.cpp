#include <dmsdk/sdk.h>
#include <dmsdk/gamesys/components/comp_collection_proxy.h>
#include <dmsdk/gamesys/components/comp_factory.h>
#include <dmsdk/dlib/array.h>

#include <stddef.h>
#include <string.h>
#include <stdlib.h> // rand()/RAND_MAX

// enum GameState
// {
//     STATE_MENU,
//     STATE_GAME,
// };

struct GameCtx
{
    dmResource::HFactory        m_Factory;
    dmConfigFile::HConfig       m_ConfigFile;

    dmGameObject::HCollection   m_MainCollection;
    dmGameObject::HCollection   m_LevelCollection;

    dmGameSystem::HCollectionProxyWorld     m_LevelCollectionProxyWorld;
    dmGameSystem::HCollectionProxyComponent m_LevelCollectionProxy;

    dmGameSystem::HFactoryWorld         m_FactoryWorld;
    dmGameSystem::HFactoryComponent     m_StarFactory;

    dmArray<dmGameObject::HInstance>    m_Stars;

    uint64_t    m_LastFrameTime;
    float       m_SpawnTimer;
    float       m_SpawnTimerInterval;

    int         m_ScreenWidth;
    int         m_ScreenHeight;

    //GameState   m_State;
    //int         m_SubState;

    bool    m_IsInitialized;
    bool    m_HasError;
    bool    m_CollectionLoading;
    bool    m_LevelInitialized;

    GameCtx()
    {
        memset(this, 0, sizeof(*this));
    }
};

GameCtx* g_Ctx = 0; // can be created in AppInit and passed around. Global here for clarity/readability


dmExtension::Result AppInit(dmExtension::AppParams* params)
{
    return dmExtension::RESULT_OK;
}

dmExtension::Result AppFinalize(dmExtension::AppParams* params)
{
    return dmExtension::RESULT_OK;
}

static void CollectionLoadCallback(const char* path, dmGameObject::Result result, void* user_ctx)
{
    dmLogInfo("CollectionLoadCallback: '%s' %d", path, (int)result);

    GameCtx* ctx = (GameCtx*)user_ctx;
    ctx->m_CollectionLoading = false;
    ctx->m_HasError = result != dmGameObject::RESULT_OK;
    if (ctx->m_HasError)
    {
        dmLogError("Failed to load collection '%s'", path);
        return;
    }

    dmResource::Result res = dmResource::Get(ctx->m_Factory, path, (void**) &ctx->m_LevelCollection);
    if (dmResource::RESULT_OK != res)
    {
        dmLogError("Failed to get level collection '%s'", path);
        ctx->m_IsInitialized = true;
        ctx->m_HasError = true;
        return;
    }
    assert(ctx->m_LevelCollection != 0);

    uint32_t factory_type_index = dmGameObject::GetComponentTypeIndex(ctx->m_LevelCollection, dmHashString64("factoryc"));

    dmhash_t go_name = dmHashString64("/factories");
    dmGameObject::HInstance go = dmGameObject::GetInstanceFromIdentifier(ctx->m_LevelCollection, go_name);
    if (go == 0)
    {
        dmLogError("Main collection does not have a game object named %s", dmHashReverseSafe64(go_name));
        ctx->m_HasError = true;
        return;
    }

    uint32_t component_type_index;
    dmGameObject::Result r = dmGameObject::GetComponent(go, dmHashString64("starfactory"), &component_type_index, (dmGameObject::HComponent*)&ctx->m_StarFactory, (dmGameObject::HComponentWorld*)&ctx->m_FactoryWorld);
    assert(dmGameObject::RESULT_OK == r);
    assert(factory_type_index == component_type_index);

    // Init the collection
    dmGameSystem::CompCollectionProxyInitialize(ctx->m_LevelCollectionProxyWorld, ctx->m_LevelCollectionProxy);
    // Spawning is currently calling initialize on the game object instance, as does the collection init
    // So we need to spawn after the collection init

    // Init and enable the collection
    dmGameSystem::CompCollectionProxyEnable(ctx->m_LevelCollectionProxyWorld, ctx->m_LevelCollectionProxy);
}

static void SpawnStar(GameCtx* ctx, dmGameSystem::HFactoryComponent factory)
{
    if (ctx->m_Stars.Full())
    {
        dmLogWarning("Stars buffer is full, skipping spawn of new star.");
        return;
    }

    uint32_t index = dmGameObject::AcquireInstanceIndex(ctx->m_LevelCollection);
    if (index == dmGameObject::INVALID_INSTANCE_POOL_INDEX)
    {
        dmLogError("Gameobject buffer is full. See `collection.max_instances` in game.project");
        ctx->m_HasError = true;
        return;
    }

    dmhash_t starid = dmGameObject::ConstructInstanceId(index);

    float y = ctx->m_ScreenHeight * (rand() / (float)RAND_MAX);
    dmVMath::Point3 position(ctx->m_ScreenWidth/2, y, 0.1f);
    dmVMath::Quat rotation(0,0,0,1);
    dmVMath::Vector3 scale(2.0f,2.0f,2.0f);

    dmGameObject::HPropertyContainer properties = 0;

    dmGameObject::HInstance instance = dmGameSystem::CompFactorySpawn(ctx->m_FactoryWorld, factory, ctx->m_LevelCollection,
                                                            index, starid, position, rotation, scale, properties);

    ctx->m_Stars.Push(instance);
}

static void MoveStars(GameCtx* ctx, float dt)
{
    float speed = -260;
    float left_side_limit = -32;
    for (uint32_t i = 0; i < ctx->m_Stars.Size(); ++i)
    {
        dmGameObject::HInstance instance = ctx->m_Stars[i];

        dmVMath::Point3 position = dmGameObject::GetPosition(instance);
        position.setX( position.getX() + speed * dt);

        // We could of course delete the object, and spawn a new one
        // but it feel nicer to just move it
        if (position.getX() < left_side_limit)
        {
            position.setX( position.getX() + ctx->m_ScreenWidth + 32);
        }
        dmGameObject::SetPosition(instance, position);
    }
}

static void LoadCollection(GameCtx* ctx, dmhash_t path, dmhash_t fragment)
{
    dmLogInfo("LoadCollection: %s", dmHashReverseSafe64(path));

    assert(ctx->m_CollectionLoading == false);
    assert(ctx->m_LevelCollection == 0);

    // Get the name of the level we want to load collection:

    //  Alternative 1: Find the component, and invoke load on it
    //          https://github.com/defold/defold/blob/dev/engine/gamesys/src/gamesys/scripts/script_collection_factory.cpp#L178-L214
    //      Compat: Here we don't have a C++ api, and to load the collection proxy, we'd use a component

    //      dmMessage::URL receiver;
    //      dmScript::GetComponentFromLua(L, 1, COLLECTION_FACTORY_EXT, (void**)&world, (void**)&component, &receiver);
    //      dmGameSystem::CompCollectionFactoryLoad(world, component, callback_ref, self_ref, url_ref);

    //  Alternative 2: Create a ProxyComponent dynamically, and invoke load on it.
    //      Compat:
    //          * We'd need the collection have support dynamic number of components
    //          * We'd need to be able to create components at runtime

    //      Compat: Supported (private gameobject.h)
    uint32_t proxy_type_index = dmGameObject::GetComponentTypeIndex(ctx->m_MainCollection, dmHashString64("collectionproxyc"));
    // dmGameSystem::CollectionFactoryWorld* proxy_world = dmGameObject::GetWorld(g_Ctx.m_MainCollection, proxy_type_index);

    // dmResource::ResourceType resource_type;
    // dmResource::Result resource_res = dmResource::GetTypeFromExtension(params->m_Factory, "collectionproxyc", &resource_type);

    // assuming the collection proxy compponents are under go named "/levels"
    //      Compat: Supported (private gameobject.h)
    dmhash_t go_name = dmHashString64("/levels");
    dmGameObject::HInstance go = dmGameObject::GetInstanceFromIdentifier(ctx->m_MainCollection, go_name);
    if (go == 0)
    {
        dmLogError("Main collection does not have a game object named %s", dmHashReverseSafe64(go_name));
        ctx->m_HasError = true;
        return;
    }

    //dmGameSystem::CollectionFactoryComponent* component;
    // dmScript::GetComponentFromLua(L, 1, COLLECTION_FACTORY_EXT, (void**)&world, (void**)&component, &receiver);

    // Compat: it sort of exists in the dmScript
    //      We want to have a more direct function, in the dmGameObject space

    // Use the fragment name to get the component info
    dmGameObject::HComponentWorld world;
    dmGameObject::HComponent component;
    uint32_t component_type_index;
    dmGameObject::Result r = dmGameObject::GetComponent(go, dmHashString64("level1"), &component_type_index, &component, &world);
    assert(dmGameObject::RESULT_OK == r);
    assert(proxy_type_index == component_type_index);

    // Alternative: Message the component directly. Needs more on hands with message structs.

    // Start the loading of the proxy
    //      Lua Scripting api: dmGameSystem::CompCollectionFactoryLoad(proxy_world, component, callback_ref, self_ref, url_ref);

    ctx->m_LevelCollectionProxyWorld = (dmGameSystem::HCollectionProxyWorld)world;
    ctx->m_LevelCollectionProxy = (dmGameSystem::HCollectionProxyComponent)component;

    //typedef void (*ProxyLoadCallback)(const char* path, bool loaded_ok, void* user_ctx);
    //dmGameObject::Result CompCollectionProxyLoad(HCollectionProxyWorld world, HCollectionProxyComponent proxy, ProxyLoadCallback cbk, void* cbk_ctx);
    r = dmGameSystem::CompCollectionProxyLoadAsync(ctx->m_LevelCollectionProxyWorld, ctx->m_LevelCollectionProxy, CollectionLoadCallback, (void*)ctx);
    assert(dmGameObject::RESULT_OK == r);

    //dmGameSystem::CompCollectionProxyAsyncLoad((HCollectionProxyWorld)world, (HCollectionProxyComponent)component, ProxyAsyncLoad, void* callback_ctx);
    ctx->m_CollectionLoading = true;
    // we shouldn't block here, as we're in the Init function
}

static void UnloadCollection(GameCtx* ctx, dmhash_t path, dmhash_t fragment)
{
    dmLogInfo("UnloadCollection: %s", dmHashReverseSafe64(path));

    assert(ctx->m_CollectionLoading == false);
    assert(ctx->m_LevelCollection != 0);
}

static void InitGame(GameCtx* ctx)
{
    dmLogInfo("InitGame");

    const char* main_collection_path = dmConfigFile::GetString(ctx->m_ConfigFile, "bootstrap.main_collection", 0);

    ctx->m_ScreenWidth = dmConfigFile::GetInt(ctx->m_ConfigFile, "display.width", 800);
    ctx->m_ScreenHeight = dmConfigFile::GetInt(ctx->m_ConfigFile, "display.height", 600);

    // Get the main collection
    // Needs to be called after the resource types have been registered
    dmResource::Result res = dmResource::Get(ctx->m_Factory, main_collection_path, (void**) &ctx->m_MainCollection);
    if (dmResource::RESULT_OK != res)
    {
        dmLogError("Failed to get main collection '%s'", main_collection_path);
        ctx->m_IsInitialized = true;
        ctx->m_HasError = true;
        return;
    }
    assert(ctx->m_MainCollection != 0);

    //ctx->m_State == STATE_MENU;

    ctx->m_LastFrameTime = 0;
    ctx->m_SpawnTimerInterval = 1;
    ctx->m_Stars.SetCapacity(16);

    LoadCollection(ctx, dmHashString64("/levels"), dmHashString64("level1"));

    ctx->m_IsInitialized = true;
}


static void ExitGame(GameCtx* ctx)
{
    dmLogInfo("ExitGame");
    if (ctx->m_MainCollection)
    {
        dmResource::Release(ctx->m_Factory, ctx->m_MainCollection);
        ctx->m_MainCollection = 0;
    }
}

static void InitLevel(GameCtx* ctx)
{
    dmLogInfo("InitLevel");

    ctx->m_SpawnTimer = ctx->m_SpawnTimerInterval;
    ctx->m_LastFrameTime = dmTime::GetTime();
}

static void UpdateGame(GameCtx* ctx)
{
    dmLogOnceInfo("UpdateGame");
    uint64_t last_time = ctx->m_LastFrameTime;
    ctx->m_LastFrameTime = dmTime::GetTime();;
    float dt = (ctx->m_LastFrameTime - last_time) / 1000000.0f;

    ctx->m_SpawnTimer -= dt;
    if (ctx->m_SpawnTimer <= 0.0f)
    {
        SpawnStar(ctx, ctx->m_StarFactory);
        ctx->m_SpawnTimer += ctx->m_SpawnTimerInterval;
    }

    MoveStars(ctx, dt);
}

dmExtension::Result Init(dmExtension::Params* params)
{
    GameCtx* ctx = new GameCtx();
    g_Ctx = ctx; // TODO: Figure out how to store it as the extension context

    ctx->m_Factory = params->m_ResourceFactory;
    ctx->m_ConfigFile = params->m_ConfigFile;
    // copy any other contexts needed

    return dmExtension::RESULT_OK;
}

dmExtension::Result Update(dmExtension::Params* params)
{
    GameCtx* ctx = g_Ctx; // TODO: get it from the context

    if (ctx->m_HasError)
    {
        return dmExtension::RESULT_OK;
    }

    if (!ctx->m_IsInitialized)
    {
        return dmExtension::RESULT_OK;
    }

    if (ctx->m_CollectionLoading)
        return dmExtension::RESULT_OK;

    if (!ctx->m_LevelInitialized)
    {
        InitLevel(ctx);
        ctx->m_LevelInitialized = true;
    }

    // TODO: No delta time in Params yet
    UpdateGame(ctx);

    return dmExtension::RESULT_OK;
}

dmExtension::Result Finalize(dmExtension::Params* params)
{
    return dmExtension::RESULT_OK;
}

static void OnEvent(dmExtension::Params* params, const dmExtension::Event* event)
{
    if (event->m_Event == dmExtension::EVENT_ID_ENGINE_INITIALIZED)
    {
        InitGame(g_Ctx);
    }
    else if (event->m_Event == dmExtension::EVENT_ID_ENGINE_DELETE)
    {
        ExitGame(g_Ctx);
    }
}


DM_DECLARE_EXTENSION(cppgame, "GAME", AppInit, AppFinalize, Init, Update, OnEvent, Finalize)
