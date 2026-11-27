<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="ru_RU">
    <context>
        <name>NativeController</name>
            <message><source>Could not open Favorites.</source><translation>Не удалось открыть Избранное.</translation></message>
<message>
            <source>Server address must be a valid HTTPS URL</source>
            <translation>Адрес сервера должен быть корректным HTTPS URL</translation>
        </message>
        <message>
            <source>Add server</source>
            <translation>Добавить сервер</translation>
        </message>
        <message>
            <source>Could not load organization settings. Check the server address and try again.</source>
            <translation>Не удалось загрузить данные организации. Проверьте адрес сервера и повторите попытку.</translation>
        </message>
        <message>
            <source>Enter your organization address first.</source>
            <translation>Сначала введите адрес вашей организации.</translation>
        </message>
        <message>
            <source>Choose a sign-in method or enter your email/login and password.</source>
            <translation>Выберите способ входа или введите email/логин и пароль.</translation>
        </message>
        <message>
            <source>Sign in to an account for the organization and project in this link.</source>
            <translation>Войдите в аккаунт организации и проекта, указанных в ссылке.</translation>
        </message>
    </context>
    <context>
        <name>SessionSwitcher</name>
        <message>
            <source>Select account</source>
            <translation>Выбрать аккаунт</translation>
        </message>
        <message>
            <source>Current server</source>
            <translation>Текущий сервер</translation>
        </message>
        <message>
            <source>Logout from organization</source>
            <translation>Выйти из организации</translation>
        </message>
        <message>
            <source>Log out from %1?</source>
            <translation>Выйти из организации %1?</translation>
        </message>
        <message>
            <source>Add server</source>
            <translation>Добавить сервер</translation>
        </message>
        <message>
            <source>Cancel</source>
            <translation>Отмена</translation>
        </message>
        <message>
            <source>Log out</source>
            <translation>Выйти</translation>
        </message>
    </context>
    <context>
        <name>ComposerMode</name>
        <message>
            <source>Preview</source>
            <translation>Предпросмотр</translation>
        </message>
        <message>
            <source>Write</source>
            <translation>Написание</translation>
        </message>
    </context>
    <context>
        <name>NativeUpdateInstalledToast</name>
        <!-- Issue #634 post-install update confirmation (docs/NATIVE_DESKTOP_UPDATER_TASKBAR_PROGRESS.md) -->
        <message><source>Update %1 installed successfully</source><translation>Обновление до версии %1 успешно установлено</translation></message>
    </context>
    <context>
        <name>Main</name>
        <!-- Issue #631 downloads.* reference keys (docs/NATIVE_DESKTOP_ATTACHMENT_DOWNLOAD_STATE.md) -->
        <message><source>Downloading…</source><translation>Скачивание…</translation></message>
        <message><source>Downloading… %1%</source><translation>Скачивание… %1%</translation></message>
        <!-- Issue #629 attachment card accessible name (docs/NATIVE_DESKTOP_ATTACHMENT_CARD_HITAREA.md).
             The bare "File" source is not added here: the Main context already
             carries it (the extension fallback), and a duplicate entry trips
             the catalog validator in native-locale-test. -->
        <message><source>File: %1</source><translation>Файл: %1</translation></message>
        <message><source>Open</source><translation>Открыть</translation></message>
        <message><source>Failed</source><translation>Ошибка</translation></message>
        <message><source>Download again</source><translation>Скачать снова</translation></message>
        <message><source>Handed off to browser</source><translation>Передано браузеру</translation></message>
        <message><source>Cancelled</source><translation>Отменено</translation></message>
        <message><source>Folder list</source><translation>Список папок</translation></message>
        <!-- Issue #847 cross-folder search sections (docs/NATIVE_DESKTOP_SIDEBAR_SEARCH.md):
             sidebar.allFoldersResults + sidebar.noMatchesInFolder reference keys. -->
        <message><source>All folders</source><translation>Все папки</translation></message>
        <message><source>No matches in this folder</source><translation>В этой папке совпадений нет</translation></message>
        <message><source>Other topics</source><translation>Остальные темы</translation></message>
        <message><source>Report a problem</source><translation>Сообщить о проблеме</translation></message>
        <message><source>Search cached messages</source><translation>Поиск в сохранённых сообщениях</translation></message>
        <message>
            <source>Nothing to preview yet</source>
            <translation>Пока нечего предпросматривать</translation>
        </message>
        <message>
            <source>AI assistant</source>
            <translation>AI assistant</translation>
        </message>
        <message>
            <source>AI features are temporarily unavailable in your organization.</source>
            <translation>AI-функции в вашей организации не подключены.</translation>
        </message>
        <message>
            <source>Current server</source>
            <translation>Текущий сервер</translation>
        </message>
        <message>
            <source>Clear cache</source>
            <translation>Очистить кэш</translation>
        </message>
        <message>
            <source>Message cache, local settings, then reload</source>
            <translation>Кэш сообщений, локальные настройки, перезагрузка</translation>
        </message>
        <message>
            <source>Clear message cache and all local settings? Your sign-in and saved organizations will be kept. The app will reload.</source>
            <translation>Очистить кэш сообщений и все локальные настройки? Вход и сохранённые организации останутся. Приложение перезагрузится.</translation>
        </message>
    <message><source>Poll settings</source><translation>Настройки опроса</translation></message>
    <message><source>Close poll</source><translation>Завершить опрос</translation></message>
    <message>
        <source>Profile is still loading. Try again in a moment.</source>
        <translation>Профиль ещё загружается. Попробуйте через пару секунд.</translation>
    </message>
    <message>
        <source>Could not create chat. Try again.</source>
        <translation>Не удалось создать чат. Попробуйте ещё раз.</translation>
    </message>
        <!-- Issue #97 (docs/NATIVE_DESKTOP_SIDEBAR_MUTE_INDICATORS.md): the stream
             menu Notifications section, measured from workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af
             packages/web/src/i18n/locales/ru.json (channel.* keys). -->
        <message>
            <source>Show options</source>
            <translation>Показать варианты</translation>
        </message>
        <message>
            <source>email@example.com or login</source>
            <translation>email@example.com или логин</translation>
        </message>
        <message>
            <source>Email or login</source>
            <translation>Email или логин</translation>
        </message>
        <message><source>Notifications</source><translation>Уведомления</translation></message>
        <message><source>Mentions only</source><translation>Только при упоминаниях</translation></message>
        <message><source>Muted</source><translation>Без уведомлений</translation></message>
        <message><source>All messages</source><translation>Все сообщения</translation></message>
        <message><source>Diagnostics</source><translation>Диагностика</translation></message>
        <message><source>Account deactivated</source><translation>Пользователь заблокирован</translation></message>
        <!-- Issue #88 (docs/NATIVE_DESKTOP_DM_HEADER_STATUS.md): reference
             chat.typing and presence.lastSeen plus a11y.openUserProfile
             wording at workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af. -->
        <message><source>Typing</source><translation>Печатает</translation></message>
        <message><source>last seen %1</source><translation>был(а) %1</translation></message>
        <message><source>Open profile: %1</source><translation>Открыть профиль: %1</translation></message>
        <!-- Issue #109 (docs/NATIVE_DESKTOP_MENTION_CHIPS.md): mention chip accessible
             name, same a11y.openUserProfile reference wording as above. -->
        <message><source>Open profile for %1</source><translation>Открыть профиль: %1</translation></message>
        <message><source>Select</source><translation>Выбрать</translation></message>
        <message><source>Deselect</source><translation>Снять выделение</translation></message>
        <message><source>Selected: %1</source><translation>Выбрано: %1</translation></message>
        <message><source>Forward</source><translation>Переслать</translation></message>
        <message>
            <source>Logging in…</source>
            <translation>Вход…</translation>
        </message>
        <message>
            <source>Enter the six-digit code from your authenticator app.</source>
            <translation>Введите шестизначный код из приложения-аутентификатора.</translation>
        </message>
        <message>
            <source>Authentication code</source>
            <translation>Код подтверждения</translation>
        </message>
        <message>
            <source>Confirm code</source>
            <translation>Подтвердить код</translation>
        </message>
        <message><source>Expand topics</source><translation>Развернуть темы</translation></message>
        <message><source>General Chat: %1</source><translation>Общий чат: %1</translation></message>
        <message><source>Collapse topics</source><translation>Свернуть темы</translation></message>
        <message><source>Hide extra topics</source><translation>Скрыть лишние темы</translation></message>
        <message><source>Show completed topics (%1)</source><translation>Показать завершённые темы (%1)</translation></message>
        <message><source>Show more topics (%1)</source><translation>Показать ещё темы (%1)</translation></message>
        <message><source>Could not open this profile. It may be unavailable or you may not have access.</source><translation>Не удалось открыть профиль. Возможно, он недоступен или у вас нет доступа.</translation></message>
        <message>
            <source>Uploading · %1%</source>
            <translation>Загрузка · %1%</translation>
        </message>
        <message>
            <source>Uploading %1: %2%</source>
            <translation>Загрузка %1: %2%</translation>
        </message>
        <message>
            <source>Cancel upload of %1</source>
            <translation>Отменить загрузку %1</translation>
        </message>
        <message>
            <source>Retry upload of %1</source>
            <translation>Повторить загрузку %1</translation>
        </message>
        <message>
            <source>Attached files</source>
            <translation>Прикреплённые файлы</translation>
        </message>
        <message>
            <source>Checking file</source>
            <translation>Проверка файла</translation>
        </message>
        <message>
            <source>Waiting to upload</source>
            <translation>Ожидает загрузки</translation>
        </message>
        <message><source>File</source><translation>Файл</translation></message>
        <message><source>Image</source><translation>Изображение</translation></message>
        <message><source>Video</source><translation>Видео</translation></message>
        <message><source>Remove %1</source><translation>Удалить %1</translation></message>
        <message><source>Remove image from text</source><translation>Удалить изображение из текста</translation></message>
        <message><source>Drag image into message</source><translation>Перетащить изображение в сообщение</translation></message>
        <message><source>Upload files from clipboard?</source><translation>Загрузить файлы из буфера обмена?</translation></message>
        <message><source>Preparing files…</source><translation>Подготовка файлов…</translation></message>
        <message><source>Check the file paths before uploading.</source><translation>Проверьте пути к файлам перед загрузкой.</translation></message>
        <message><source>Upload</source><translation>Загрузить</translation></message>
        <message>
            <source>Unread messages</source>
            <translation>Непрочитанные сообщения</translation>
        </message>
        <message>
            <source>Unread messages • %1</source>
            <translation>Непрочитанные сообщения • %1</translation>
        </message>
        <message>
            <source>Scroll to bottom</source>
            <translation>Прокрутить вниз</translation>
        </message>
        <message>
            <source>Scroll to bottom, %1 unread</source>
            <translation>Прокрутить вниз, непрочитанных: %1</translation>
        </message>
        <message>
            <source>Call</source>
            <translation>Позвонить</translation>
        </message>
        <message>
            <source>Add server</source>
            <translation>Добавить сервер</translation>
        </message>
        <message>
            <source>Back</source>
            <translation>Назад</translation>
        </message>
        <message>
            <source>Could not load organization settings. Check the server address and try again.</source>
            <translation>Не удалось загрузить данные организации. Проверьте адрес сервера и повторите попытку.</translation>
        </message>
        <message>
            <source>Connect to server</source>
            <translation>Подключение к серверу</translation>
        </message>
        <message>
            <source>Enter your organization address first.</source>
            <translation>Сначала укажите адрес вашей организации.</translation>
        </message>
        <message>
            <source>Choose a sign-in method or enter your email/login and password.</source>
            <translation>Выберите способ входа или введите email/логин и пароль.</translation>
        </message>
        <message>
            <source>Choose the project you want to open.</source>
            <translation>Выберите проект, который хотите открыть.</translation>
        </message>
        <message>
            <source>Project</source>
            <translation>Проект</translation>
        </message>
        <message>
            <source>Select a project</source>
            <translation>Выберите проект</translation>
        </message>
        <message>
            <source>Login</source>
            <translation>Войти</translation>
        </message>
        <message>
            <source>Your account does not have access to any projects in this organization.</source>
            <translation>У вашего аккаунта нет доступа к проектам этой организации.</translation>
        </message>
        <message>
            <source>Server address</source>
            <translation>Адрес сервера</translation>
        </message>
        <message>
            <source>https://chat.example.com</source>
            <translation>https://chat.example.com</translation>
        </message>
        <message>
            <source>Or connect to our public service.</source>
            <translation>Или можете подключиться к нашему публичному сервису.</translation>
        </message>
        <message>
            <source>Loading organization settings…</source>
            <translation>Загружаем настройки организации…</translation>
        </message>
        <message>
            <source>Next</source>
            <translation>Далее</translation>
        </message>
        <message>
            <source>online</source>
            <translation>В сети</translation>
        </message>
        <message>
            <source>away</source>
            <translation>Отошёл</translation>
        </message>
        <message>
            <source>offline</source>
            <translation>Не в сети</translation>
        </message>
        <message>
            <source>Select topic:</source>
            <translation>Выберите тему:</translation>
        </message>
        <message>
            <source>New topic</source>
            <translation>Новая тема</translation>
        </message>
        <message>
            <source>Create topic</source>
            <translation>Создать тему</translation>
        </message>
        <message>
            <source>Topic name</source>
            <translation>Название темы</translation>
        </message>
        <message>
            <source>Select channel</source>
            <translation>Выберите канал</translation>
        </message>
        <message>
            <source>Select an option</source>
            <translation>Выберите значение</translation>
        </message>
        <message>
            <source>No channels found</source>
            <translation>Каналы не найдены</translation>
        </message>
        <message>
            <source>No options available</source>
            <translation>Нет доступных вариантов</translation>
        </message>
        <message>
            <source>Create</source>
            <translation>Создать</translation>
        </message>
        <message>
            <source>Cancel</source>
            <translation>Отмена</translation>
        </message>
        <message>
            <source>Close</source>
            <translation>Закрыть</translation>
        </message>
        <message>
            <source>Reply</source>
            <translation>Ответить</translation>
        </message>
        <message>
            <source>Reply: %1</source>
            <translation>Ответить: %1</translation>
        </message>
        <message>
            <source>Close: %1</source>
            <translation>Закрыть: %1</translation>
        </message>
        <message>
            <source>Quote</source>
            <translation>Цитата</translation>
        </message>
        <message>
            <source>Message unavailable</source>
            <translation>Сообщение недоступно</translation>
        </message>
        <message>
            <source>Loading messages</source>
            <translation>Загрузка сообщений</translation>
        </message>
        <message>
            <source>Copy text</source>
            <translation>Копировать текст</translation>
        </message>
        <message>
            <source>Copy image</source>
            <translation>Копировать изображение</translation>
        </message>
        <message>
            <source>Copy link</source>
            <translation>Копировать ссылку</translation>
        </message>
        <message>
            <source>Copy</source>
            <translation>Копировать</translation>
        </message>
        <message>
            <source>Copy message link</source>
            <translation>Скопировать ссылку</translation>
        </message>
        <message>
            <source>Copied</source>
            <translation>Скопировано</translation>
        </message>
        <message>
            <source>Failed to copy</source>
            <translation>Не удалось скопировать</translation>
        </message>
        <message>
            <source>Updating disk cache…</source>
            <translation>Обновление дискового кэша…</translation>
        </message>
        <message>
            <source>Requested: %1 GB · Applied: %2 GB</source>
            <translation>Задано: %1 ГБ · Применено: %2 ГБ</translation>
        </message>
        <message>
            <source>Requested: %1 GB</source>
            <translation>Задано: %1 ГБ</translation>
        </message>
        <message>
            <source>Used: %1</source>
            <translation>Занято: %1</translation>
        </message>
        <message>
            <source>Disk cache is unavailable.</source>
            <translation>Дисковый кэш недоступен.</translation>
        </message>
        <message>
            <source>Workspace</source>
            <translation>Workspace</translation>
        </message>
        <message>
            <source>Choose a project</source>
            <translation>Выберите проект</translation>
        </message>
        <message>
            <source>Sign in to your organization</source>
            <translation>Войдите в свою организацию</translation>
        </message>
        <message>
            <source>Saved accounts on this device</source>
            <translation>Сохранённые аккаунты на этом устройстве</translation>
        </message>
        <message>
            <source>Continue</source>
            <translation>Продолжить</translation>
        </message>
        <message>
            <source>Continue as %1 in %2</source>
            <translation>Продолжить как %1 в %2</translation>
        </message>
        <message>
            <source>Remove</source>
            <translation>Удалить</translation>
        </message>
        <message>
            <source>Remove saved account %1</source>
            <translation>Удалить сохранённый аккаунт %1</translation>
        </message>
        <message>
            <source>Forget this account on this device</source>
            <translation>Забыть этот аккаунт на этом устройстве</translation>
        </message>
        <message>
            <source>Organization URL</source>
            <translation>URL организации</translation>
        </message>
        <message>
            <source>Username or email</source>
            <translation>Имя пользователя или email</translation>
        </message>
        <message>
            <source>No account?</source>
            <translation>Нет аккаунта?</translation>
        </message>
        <message>
            <source>Register</source>
            <translation>Зарегистрироваться</translation>
        </message>
        <message>
            <source>Password</source>
            <translation>Пароль</translation>
        </message>
        <message>
            <source>One-time code, if required</source>
            <translation>Одноразовый код, если требуется</translation>
        </message>
        <message>
            <source>One-time code</source>
            <translation>Одноразовый код</translation>
        </message>
        <message>
            <source>Signing in…</source>
            <translation>Вход…</translation>
        </message>
        <message>
            <source>Sign in</source>
            <translation>Войти</translation>
        </message>
        <message>
            <source>Back to sign in</source>
            <translation>Назад к входу</translation>
        </message>
        <message>
            <source>Organization</source>
            <translation>Организация</translation>
        </message>
        <message>
            <source>Account and appearance</source>
            <translation>Аккаунт и внешний вид</translation>
        </message>
        <message>
            <source>Chats and channels</source>
            <translation>Чаты и каналы</translation>
        </message>
        <message>
            <source>Calls</source>
            <translation>Звонки</translation>
        </message>
        <message>
            <source>Calendar</source>
            <translation>Календарь</translation>
        </message>
        <message>
            <source>Mail</source>
            <translation>Почта</translation>
        </message>
        <message>
            <source>This section is temporarily unavailable in web mode.</source>
            <translation>Раздел временно недоступен в веб-режиме.</translation>
        </message>
        <message>
            <source>Profile</source>
            <translation>Профиль</translation>
        </message>
        <message>
            <source>Use light appearance</source>
            <translation>Использовать светлое оформление</translation>
        </message>
        <message>
            <source>Use dark appearance</source>
            <translation>Использовать тёмное оформление</translation>
        </message>
        <message>
            <source>Light</source>
            <translation>Светлая</translation>
        </message>
        <message>
            <source>Dark</source>
            <translation>Тёмная</translation>
        </message>
        <message>
            <source>System</source>
            <translation>Системная</translation>
        </message>
        <message>
            <source>Orange Warm</source>
            <translation>Тёплый оранжевый</translation>
        </message>
        <message>
            <source>Blue Cold</source>
            <translation>Холодный синий</translation>
        </message>
        <message>
            <source>Blue Mist</source>
            <translation>Голубая дымка</translation>
        </message>
        <message>
            <source>Emerald Chat</source>
            <translation>Изумрудный чат</translation>
        </message>
        <message>
            <source>Language</source>
            <translation>Язык</translation>
        </message>
        <message>
            <source>English</source>
            <translation>English</translation>
        </message>
        <message>
            <source>Русский</source>
            <translation>Русский</translation>
        </message>
        <message>
            <source>Disk cache (GB)</source>
            <translation>Кэш на диске (ГБ)</translation>
        </message>
        <message>
            <source>Disk cache limit in GB</source>
            <translation>Лимит кэша на диске в ГБ</translation>
        </message>
        <message>
            <source>Refresh</source>
            <translation>Обновить</translation>
        </message>
        <message>
            <source>Sign out</source>
            <translation>Выйти</translation>
        </message>
        <message>
            <source>All chats</source>
            <translation>Все</translation>
        </message>
        <message>
            <source>Personal</source>
            <translation>Личные</translation>
        </message>
        <message>
            <source>Channels</source>
            <translation>Каналы</translation>
        </message>
        <message>
            <source>Add folder</source>
            <translation>Добавить папку</translation>
        </message>
        <message>
            <source>Display vertically</source>
            <translation>Отобразить вертикально</translation>
        </message>
        <message>
            <source>Display horizontally</source>
            <translation>Отобразить горизонтально</translation>
        </message>
        <message>
            <source>Show system folders</source>
            <translation>Показать системные папки</translation>
        </message>
        <message>
            <source>Hide system folders</source>
            <translation>Скрыть системные папки</translation>
        </message>
        <message>
            <source>Choose a conversation</source>
            <translation>Выберите беседу</translation>
        </message>
        <message numerus="yes">
            <source>%n member</source>
            <translation>
                <numerusform>%n участник</numerusform>
                <numerusform>%n участника</numerusform>
                <numerusform>%n участников</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>%n members</source>
            <translation>
                <numerusform>%n участник</numerusform>
                <numerusform>%n участника</numerusform>
                <numerusform>%n участников</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>%n online</source>
            <translation>
                <numerusform>%n в сети</numerusform>
                <numerusform>%n в сети</numerusform>
                <numerusform>%n в сети</numerusform>
            </translation>
        </message>
        <message>
            <source>Conversation information is not implemented yet</source>
            <translation>Информация о беседе ещё не реализована</translation>
        </message>
        <message>
            <source>Retry</source>
            <translation>Повторить</translation>
        </message>
        <message>
            <source>Load older messages</source>
            <translation>Загрузить более ранние сообщения</translation>
        </message>
        <message>
            <source>Load newer messages</source>
            <translation>Загрузить более новые сообщения</translation>
        </message>
        <message>
            <source>Loading…</source>
            <translation>Загрузка…</translation>
        </message>
        <message>
            <source>No messages to display</source>
            <translation>Нет сообщений для отображения</translation>
        </message>
        <message>
            <source>Loading message context</source>
            <translation>Загружаем контекст сообщения</translation>
        </message>
        <message>
            <source>Could not open the selected message</source>
            <translation>Не удалось открыть выбранное сообщение</translation>
        </message>
        <message>
            <source>The selected message is no longer available in the current workspace.</source>
            <translation>Выбранное сообщение больше недоступно в текущем рабочем пространстве.</translation>
        </message>
        <message>
            <source>Retry opening message</source>
            <translation>Повторить открытие сообщения</translation>
        </message>
        <message>
            <source>Check your reaction ownership</source>
            <translation>Проверить, добавлена ли вами реакция</translation>
        </message>
        <message>
            <source>Remove your reaction</source>
            <translation>Удалить вашу реакцию</translation>
        </message>
        <message>
            <source>Add your reaction</source>
            <translation>Добавить вашу реакцию</translation>
        </message>
        <message>
            <source>This provider reaction cannot be added</source>
            <translation>Эту реакцию провайдера нельзя добавить</translation>
        </message>
        <message>
            <source>Reaction not confirmed</source>
            <translation>Реакция не подтверждена</translation>
        </message>
        <message>
            <source>Updating reaction…</source>
            <translation>Обновление реакции…</translation>
        </message>
        <message>
            <source>Check status</source>
            <translation>Проверить статус</translation>
        </message>
        <message>
            <source>Check without sending the reaction again</source>
            <translation>Проверить без повторной отправки реакции</translation>
        </message>
        <message>
            <source>This topic is resolved.</source>
            <translation>Эта тема решена.</translation>
        </message>
        <message>
            <source>Remove the mark</source>
            <translation>Снимите отметку</translation>
        </message>
        <message>
            <source>to send a message</source>
            <translation>чтобы отправить сообщение</translation>
        </message>
        <message>
            <source>Show formatting tools</source>
            <translation>Показать инструменты форматирования</translation>
        </message>
        <message>
            <source>Hide formatting tools</source>
            <translation>Скрыть инструменты форматирования</translation>
        </message>
        <message>
            <source>Link</source>
            <translation>Ссылка</translation>
        </message>
        <message>
            <source>Bold</source>
            <translation>Жирный</translation>
        </message>
        <message>
            <source>Italic</source>
            <translation>Курсив</translation>
        </message>
        <message>
            <source>Strikethrough</source>
            <translation>Зачёркнутый</translation>
        </message>
        <message>
            <source>Numbered list</source>
            <translation>Нумерованный список</translation>
        </message>
        <message>
            <source>Bulleted list</source>
            <translation>Маркированный список</translation>
        </message>
        <message>
            <source>Code block</source>
            <translation>Блок кода</translation>
        </message>
        <message>
            <source>Code</source>
            <translation>Код</translation>
        </message>
        <message>
            <source>Spoiler</source>
            <translation>Спойлер</translation>
        </message>
        <message>
            <source>link text</source>
            <translation>текст ссылки</translation>
        </message>
        <message>
            <source>Write a message…</source>
            <translation>Написать сообщение…</translation>
        </message>
        <message>
            <source>Encrypted messaging is not implemented yet</source>
            <translation>Шифрование сообщений ещё не реализовано</translation>
        </message>
        <message>
            <source>Choose a topic to write a message</source>
            <translation>Выберите тему, чтобы написать сообщение</translation>
        </message>
        <message>
            <source>Message</source>
            <translation>Написать</translation>
        </message>
        <message>
            <source>Attach files</source>
            <translation>Прикрепить файлы</translation>
        </message>
        <message>
            <source>Emoji</source>
            <translation>Эмодзи</translation>
        </message>
        <message>
            <source>Emoji and stickers are not implemented yet</source>
            <translation>Эмодзи и стикеры ещё не реализованы</translation>
        </message>
        <message>
            <source>Send message</source>
            <translation>Отправить сообщение</translation>
        </message>
        <message>
            <source>Showing saved messages · Updating…</source>
            <translation>Показаны сохранённые сообщения · Обновление…</translation>
        </message>
        <message>
            <source>Showing saved chats · Updating…</source>
            <translation>Показаны сохранённые чаты · Обновление…</translation>
        </message>
        <message>
            <source>Showing saved messages · Update failed</source>
            <translation>Показаны сохранённые сообщения · Не удалось обновить</translation>
        </message>
        <message>
            <source>Showing saved chats · Update failed</source>
            <translation>Показаны сохранённые чаты · Не удалось обновить</translation>
        </message>
        <message>
            <source>The saved account could not reconnect because the service is temporarily unavailable.</source>
            <translation>Не удалось переподключить сохранённый аккаунт, потому что сервис временно недоступен.</translation>
        </message>
        <message>
            <source>Save attachment</source>
            <translation>Сохранить вложение</translation>
        </message>
        <message>
            <source>Workspace reference</source>
            <translation>Ссылка на Workspace</translation>
        </message>
        <message>
            <source>Date and time</source>
            <translation>Дата и время</translation>
        </message>
        <message>
            <source>Time</source>
            <translation>Время</translation>
        </message>
        <message>
            <source>Profiles, referenced messages and attachment previews are still under development.</source>
            <translation>Профили, переход к сообщениям и предпросмотр вложений пока в разработке.</translation>
        </message>
        <message>
            <source>Start chat</source>
            <translation>Начать чат</translation>
        </message>
        <message>
            <source>Create channel</source>
            <translation>Создать канал</translation>
        </message>
        <message>
            <source>Archive</source>
            <translation>Архив</translation>
        </message>
        <message>
            <source>This section is not implemented yet</source>
            <translation>Этот раздел пока не реализован</translation>
        </message>
        <message>
            <source>User list is currently unavailable.</source>
            <translation>Список пользователей пока недоступен.</translation>
        </message>
        <message>
            <source>Opening chat…</source>
            <translation>Открываем чат…</translation>
        </message>
        <message>
            <source>Channel name</source>
            <translation>Название канала</translation>
        </message>
        <message>
            <source>Description (optional)</source>
            <translation>Описание</translation>
        </message>
        <message>
            <source>Invite only</source>
            <translation>Только по приглашению</translation>
        </message>
        <message>
            <source>Announce channel creation</source>
            <translation>Анонсировать создание канала</translation>
        </message>
        <message>
            <source>Announcement-only channel</source>
            <translation>Канал объявлений</translation>
        </message>
        <message>
            <source>Announcement-only policy is not connected to Workspace yet.</source>
            <translation>Политика канала объявлений пока не подключена к Workspace.</translation>
        </message>
        <message>
            <source>Protected channel (E2EE)</source>
            <translation>Защищенный канал (E2EE)</translation>
        </message>
        <message>
            <source>Create encrypted chat</source>
            <translation>Создать зашифрованный чат</translation>
        </message>
        <message>
            <source>Add members</source>
            <translation>Добавить участников</translation>
        </message>
        <message>
            <source>Search users…</source>
            <translation>Поиск пользователей…</translation>
        </message>
        <message>
            <source>No results</source>
            <translation>Ничего не найдено</translation>
        </message>
        <message>
            <source>Loading members…</source>
            <translation>Загрузка участников…</translation>
        </message>
        <message>
            <source>The channel exists. Complete adding the selected members.</source>
            <translation>Канал уже создан. Завершите добавление выбранных участников.</translation>
        </message>
        <message>
            <source>Retry members</source>
            <translation>Повторить добавление</translation>
        </message>
        <message>
            <source>Add reaction: %1</source>
            <translation>Поставить реакцию: %1</translation>
        </message>
        <message>
            <source>More reactions</source>
            <translation>Другие реакции</translation>
        </message>
        <message>
            <source>Edit</source>
            <translation>Редактировать</translation>
        </message>
        <message>
            <source>Delete</source>
            <translation>Удалить</translation>
        </message>
        <message>
            <source>Edit message</source>
            <translation>Изменить сообщение</translation>
        </message>
        <message>
            <source>Message text</source>
            <translation>Текст сообщения</translation>
        </message>
        <message>
            <source>Delete this message?</source>
            <translation>Удалить это сообщение?</translation>
        </message>
        <message>
            <source>Find</source>
            <translation>Найти</translation>
        </message>
        <message>
            <source>Search</source>
            <translation>Поиск</translation>
        </message>
        <message>
            <source>Clear search</source>
            <translation>Очистить поиск</translation>
        </message>
        <message>
            <source>Today</source>
            <translation>Сегодня</translation>
        </message>
        <message>
            <source>Yesterday</source>
            <translation>Вчера</translation>
        </message>
        <message>
            <source>New chat</source>
            <translation>Новый чат</translation>
        </message>
        <message>
            <source>Inbox</source>
            <translation>Входящие</translation>
        </message>
        <message>
            <source>Favorites</source>
            <translation>Избранное</translation>
        </message>
        <message>
            <source>Starred messages</source>
            <translation>Отмеченные сообщения</translation>
        </message>
        <message>
            <source>My Activity</source>
            <translation>Моя активность</translation>
        </message>
        <message>
            <source>Marked messages</source>
            <translation>Отмеченные сообщения</translation>
        </message>
        <message>
            <source>Mentions</source>
            <translation>Упоминания</translation>
        </message>
        <message>
            <source>Drafts</source>
            <translation>Черновики</translation>
        </message>
        <message>
            <source>%1 is not implemented yet</source>
            <translation>%1 ещё не реализовано</translation>
        </message>
        <message>
            <source>Activity is not implemented yet</source>
            <translation>Активность ещё не реализована</translation>
        </message>
        <message>
            <source>Channel</source>
            <translation>Канал</translation>
        </message>
        <message>
            <source>Direct chat</source>
            <translation>Личный чат</translation>
        </message>
        <message>
            <source>Search archived chats…</source>
            <translation>Поиск архивированных чатов…</translation>
        </message>
        <message>
            <source>No archived chats found</source>
            <translation>Архивированные чаты не найдены</translation>
        </message>
        <message>
            <source>Could not unarchive the chat. Try again.</source>
            <translation>Не удалось разархивировать чат. Попробуйте ещё раз.</translation>
        </message>
        <message>
            <source>Could not load the archive.</source>
            <translation>Не удалось загрузить архив.</translation>
        </message>
        <message>
            <source>Open history: %1</source>
            <translation>Открыть историю: %1</translation>
        </message>
        <message>
            <source>Unarchive</source>
            <translation>Разархивировать</translation>
        </message>
        <message>
            <source>Unarchiving</source>
            <translation>Разархивирование</translation>
        </message>
        <message>
            <source>Could not decrypt message</source>
            <translation>Не удалось расшифровать</translation>
        </message>
        <message>
            <source>Decrypting message</source>
            <translation>Расшифровка сообщения</translation>
        </message>
        <message>
            <source>Decrypting message…</source>
            <translation>Расшифровка сообщения…</translation>
        </message>
        <message>
            <source>Encrypted key grant received</source>
            <translation>Получен зашифрованный ключ</translation>
        </message>
        <message>
            <source>Sending encrypted messages is currently unavailable</source>
            <translation>Отправка сообщений в защищённых чатах пока недоступна.</translation>
        </message>
        <message>
            <source>Encryption is unavailable for this chat or local key storage could not be opened.</source>
            <translation>Шифрование для этого чата недоступно или не удалось открыть локальное хранилище ключей.</translation>
        </message>
        <message>
            <source>Key request rejected</source>
            <translation>Запрос ключа отклонён</translation>
        </message>
        <message>
            <source>Key requested</source>
            <translation>Запрошен ключ</translation>
        </message>
        <message>
            <source>Key required</source>
            <translation>Нужен ключ</translation>
        </message>
        <message>
            <source>Keys are stored on the device. Different periods of chat history may require different keys.</source>
            <translation>Ключи хранятся на устройстве. Для разных периодов истории могут понадобиться разные ключи.</translation>
        </message>
        <message>
            <source>Message text and all links within it are protected by end-to-end encryption.</source>
            <translation>Текст сообщений и все ссылки внутри них защищены сквозным шифрованием.</translation>
        </message>
        <message>
            <source>Images, videos, and files are stored without end-to-end encryption and can be accessed by the server and storage.</source>
            <translation>Изображения, видео и файлы хранятся без сквозного шифрования и доступны серверу и хранилищу.</translation>
        </message>
        <message>
            <source>To open an attachment from the chat, you need to decrypt the message containing its link.</source>
            <translation>Чтобы открыть вложение из чата, нужно расшифровать сообщение со ссылкой на него.</translation>
        </message>
        <message>
            <source>New key announced</source>
            <translation>Объявлен новый ключ</translation>
        </message>
        <message>
            <source>Protected chat</source>
            <translation>Защищённый чат</translation>
        </message>
        <message>
            <source>Protected chat information</source>
            <translation>Особенности защищённого чата</translation>
        </message>
        <message>
            <source>Request key</source>
            <translation>Запросить ключ</translation>
        </message>
        <message>
            <source>Request key for this message</source>
            <translation>Запросить ключ для этого сообщения</translation>
        </message>
        <message>
            <source>Request the key for this generation to read the message.</source>
            <translation>Запросите ключ этого поколения, чтобы прочитать сообщение.</translation>
        </message>
        <message>
            <source>Retry opening secure key storage</source>
            <translation>Повторить открытие защищённого хранилища ключей</translation>
        </message>
        <message>
            <source>Retry sending the same key request</source>
            <translation>Повторить отправку того же запроса ключа</translation>
        </message>
        <message>
            <source>The key is available on this device</source>
            <translation>Ключ доступен на этом устройстве</translation>
        </message>
        <message>
            <source>The key request was rejected. You can send another request.</source>
            <translation>Запрос ключа отклонён. Можно отправить новый запрос.</translation>
        </message>
        <message>
            <source>Waiting for a response</source>
            <translation>Ожидание ответа</translation>
        </message>
        <message>
            <source>Without the matching key, a message cannot be read even if it has already been downloaded to the device.</source>
            <translation>Без нужного ключа сообщение нельзя прочитать, даже если оно уже загружено на устройство.</translation>
        </message>
        <message>
            <source>The security key has changed. Request the key from the channel owner to read new messages</source>
            <translation>Ключ безопасности обновился. Запросите ключ у владельца канала для просмотра новых сообщений</translation>
        </message>
        <message>
            <source>Check message status</source>
            <translation>Проверить статус сообщения</translation>
        </message>
        <message>
            <source>Retry sending the same message</source>
            <translation>Повторить отправку того же сообщения</translation>
        </message>
        <message>
            <source>You requested a security key for device “%1”</source>
            <translation>Вы запросили ключ безопасности для устройства «%1»</translation>
        </message>
        <message>
            <source>requested a security key for device “%1”</source>
            <translation>запросил(а) ключ безопасности для устройства «%1»</translation>
        </message>
        <message>
            <source>Unknown device</source>
            <translation>Неизвестное устройство</translation>
        </message>
        <message>
            <source>Approve the request on another device with the key or wait for access to be granted</source>
            <translation>Подтвердите запрос на другом устройстве с ключом или дождитесь предоставления доступа</translation>
        </message>
        <message>
            <source>This device does not have the requested key</source>
            <translation>На этом устройстве нет запрошенного ключа</translation>
        </message>
        <message>
            <source>Send key</source>
            <translation>Передать ключ</translation>
        </message>
        <message>
            <source>Reject</source>
            <translation>Отклонить</translation>
        </message>
        <message>
            <source>Key sent</source>
            <translation>Ключ отправлен</translation>
        </message>
        <message>
            <source>Request rejected.</source>
            <translation>Запрос отклонён.</translation>
        </message>
        <message>
            <source>Wait for access to be granted to read the messages</source>
            <translation>Дождитесь предоставления доступа, чтобы увидеть сообщения</translation>
        </message>
        <message>
            <source>You sent a security key for device “%1”</source>
            <translation>Вы передали ключ безопасности для устройства «%1»</translation>
        </message>
        <message>
            <source>You sent a security key to %1 for device “%2”</source>
            <translation>Вы передали ключ безопасности пользователю %1 для устройства «%2»</translation>
        </message>
        <message>
            <source>%1 sent you a security key for device “%2”</source>
            <translation>%1 передал(а) вам ключ безопасности для устройства «%2»</translation>
        </message>
        <message>
            <source>%1 sent a security key to %2 for device “%3”</source>
            <translation>%1 передал(а) ключ безопасности пользователю %2 для устройства «%3»</translation>
        </message>
        <message>
            <source>You rejected the security key request for device “%1”</source>
            <translation>Вы отклонили запрос ключа безопасности для устройства «%1»</translation>
        </message>
        <message>
            <source>You rejected the security key request from %1 for device “%2”</source>
            <translation>Вы отклонили запрос ключа безопасности пользователя %1 для устройства «%2»</translation>
        </message>
        <message>
            <source>%1 rejected your security key request for device “%2”</source>
            <translation>%1 отклонил(а) ваш запрос ключа безопасности для устройства «%2»</translation>
        </message>
        <message>
            <source>%1 rejected the security key request from %2 for device “%3”</source>
            <translation>%1 отклонил(а) запрос ключа безопасности пользователя %2 для устройства «%3»</translation>
        </message>
        <message>
            <source>%1 sent an encrypted security key</source>
            <translation>%1 отправил(а) зашифрованный ключ безопасности</translation>
        </message>
        <message>
            <source>%1 rejected the key request</source>
            <translation>%1 отклонил(а) запрос ключа</translation>
        </message>
        <message>
            <source>Recipient information is unavailable.</source>
            <translation>Информация о получателе недоступна.</translation>
        </message>
        <message>
            <source>You can request the key again</source>
            <translation>Вы можете запросить ключ повторно</translation>
        </message>
        <message>
            <source>Request</source>
            <translation>Запросить</translation>
        </message>
        <message numerus="yes">
            <source>%n event</source>
            <translation>
                <numerusform>%n событие</numerusform>
                <numerusform>%n события</numerusform>
                <numerusform>%n событий</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>%n events</source>
            <translation>
                <numerusform>%n событие</numerusform>
                <numerusform>%n события</numerusform>
                <numerusform>%n событий</numerusform>
            </translation>
        </message>
        <message>
            <source>Action required</source>
            <translation>Требуется действие</translation>
        </message>
        <message>
            <source>Awaiting response</source>
            <translation>Ожидается ответ</translation>
        </message>
        <message>
            <source>Status unavailable</source>
            <translation>Статус недоступен</translation>
        </message>
        <message>
            <source>The security key was updated</source>
            <translation>Ключ безопасности обновился</translation>
        </message>
        <message>
            <source>Request the key to read the messages</source>
            <translation>Запросите ключ для просмотра сообщений</translation>
        </message>
        <message>
            <source>Add an emoji and text to show your current availability.</source>
            <translation>Добавьте эмодзи и текст, чтобы показать текущую занятость.</translation>
        </message>
        <message>
            <source>Choose emoji</source>
            <translation>Выбрать эмодзи</translation>
        </message>
        <message>
            <source>Clear status</source>
            <translation>Очистить статус</translation>
        </message>
        <message>
            <source>Dismiss</source>
            <translation>Скрыть</translation>
        </message>
        <message>
            <source>Failed to update status</source>
            <translation>Не удалось обновить статус</translation>
        </message>
        <message>
            <source>Mark me as away</source>
            <translation>Отметить меня как отсутствующего</translation>
        </message>
        <message>
            <source>Save</source>
            <translation>Сохранить</translation>
        </message>
        <message>
            <source>Set your status</source>
            <translation>Укажите статус</translation>
        </message>
        <message>
            <source>Status</source>
            <translation>Статус</translation>
        </message>
        <message>
            <source>App version</source>
            <translation>Версия приложения</translation>
        </message>
        <message>
            <source>Failed to load your messages.</source>
            <translation>Не удалось загрузить сообщения.</translation>
        </message>
        <message>
            <source>You are offline.</source>
            <translation>Нет подключения к сети.</translation>
        </message>
        <message>
            <source>Too many requests. Retrying in %1 seconds.</source>
            <translation>Слишком много запросов. Повтор через %1 секунд.</translation>
        </message>
        <message>
            <source>The connection is degraded.</source>
            <translation>Соединение нестабильно.</translation>
        </message>
        <message>
            <source>Reconnecting...</source>
            <translation>Переподключение...</translation>
        </message>
        <message>
            <source>No results found</source>
            <translation>Ничего не найдено</translation>
        </message>
        <message>
            <source>not in channel</source>
            <translation>не в канале</translation>
        </message>
            <message><source>Partner info</source><translation>Информация о собеседнике</translation></message>
        <message><source>Channel info</source><translation>Информация о канале</translation></message>
        <message>
            <source>Information</source>
            <translation>Информация</translation>
        </message>
        <message>
            <source>Personal info</source>
            <translation>Личная информация</translation>
        </message>
        <message>
            <source>Create poll</source>
            <translation>Создать опрос</translation>
        </message>
        <message>
            <source>Review unresolved encrypted channel creations</source>
            <translation>Показать незавершённые попытки создания зашифрованных каналов</translation>
        </message>
        <message>
            <source>Unresolved encrypted channel creations</source>
            <translation>Незавершённые попытки создания зашифрованных каналов</translation>
        </message>
        <message>
            <source>Check re-adopts the key if the channel landed; discard is final and may make a late channel unreadable.</source>
            <translation>«Проверить» вернёт ключ, если канал создался; удаление окончательно — запоздавший канал нельзя будет прочитать.</translation>
        </message>
        <message>
            <source>Created: %1</source>
            <translation>Создана: %1</translation>
        </message>
        <message>
            <source>All attempts are now resolved.</source>
            <translation>Все попытки более не требуют уточнения.</translation>
        </message>
        <message>
            <source>Check</source>
            <translation>Проверить</translation>
        </message>
        <message>
            <source>Discard</source>
            <translation>Удалить</translation>
        </message>
        <message>
            <source>This channel may still appear later. If it does, it can never be read. Discard this creation attempt and its staged key?</source>
            <translation>Этот канал может появиться позже. Если это произойдёт, его нельзя будет прочитать. Удалить эту попытку создания канала и её ключ?</translation>
        </message>
        <message>
            <source>Discard key</source>
            <translation>Удалить ключ</translation>
        </message>
        <message>
            <source>Mark all as read</source>
            <translation>Отметить всё как прочитанное</translation>
        </message>
        <message>
            <source>Rename</source>
            <translation>Переименовать</translation>
        </message>
        <message>
            <source>Archive chat?</source>
            <translation>Архивировать чат?</translation>
        </message>
        <message>
            <source>Archive your conversation with “%1”? The chat will be archived for both of you.</source>
            <translation>Архивировать вашу переписку с пользователем «%1»? Чат попадёт в архив у вас обоих.</translation>
        </message>
        <message>
            <source>Archive “%1” for all participants?</source>
            <translation>Архивировать «%1» для всех участников?</translation>
        </message>
        <message>
            <source>The history will remain available in New chat → Archive. You can restore the chat there.</source>
            <translation>История останется доступна в разделе «Новый чат → Архив». Там можно восстановить чат.</translation>
        </message>
        <message>
            <source>Please wait…</source>
            <translation>Подождите…</translation>
        </message>
        <message>
            <source>Try again</source>
            <translation>Попробовать снова</translation>
        </message>
    <message>
        <source>Checking pending message…</source>
        <translation>Проверка ожидающего сообщения…</translation>
    </message>
    <message>
        <source>This message is waiting for confirmation.</source>
        <translation>Это сообщение ожидает подтверждения.</translation>
    </message>
    <message>
        <source>Discard pending message?</source>
        <translation>Удалить ожидающее сообщение?</translation>
    </message>
    <message>
        <source>The message may already have been sent. Discard its pending status and keep the current draft?</source>
        <translation>Возможно, сообщение уже отправлено. Удалить его из очереди и сохранить текущий черновик?</translation>
    </message>
    <message>
        <source>Create folder</source>
        <translation>Создать папку</translation>
    </message>
    <message>
        <source>Rename folder</source>
        <translation>Переименовать папку</translation>
    </message>
    <message>
        <source>Folder name</source>
        <translation>Название папки</translation>
    </message>
    <message>
        <source>Color</source>
        <translation>Цвет</translation>
    </message>
    <message>
        <source>Preview</source>
        <translation>Пример</translation>
    </message>
    <message>
        <source>Delete folder?</source>
        <translation>Удалить папку?</translation>
    </message>
    <message>
        <source>Are you sure you want to delete &quot;%1&quot;?</source>
        <translation>Вы уверены, что хотите удалить «%1»?</translation>
    </message>
    <message><source>Settings</source><translation>Настройки</translation></message>
    <message><source>Appearance</source><translation>Оформление</translation></message>
</context>
    <context>
        <name>NativeQuickReactions</name>
        <message>
            <source>Remove your reaction: %1</source>
            <translation>Убрать свою реакцию: %1</translation>
        </message>
        <message>
            <source>Add reaction: %1</source>
            <translation>Поставить реакцию: %1</translation>
        </message>
    </context>
    <context>
        <name>NativeReactionPicker</name>
        <message>
            <source>No results found</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>No results found</translation>
        </message>
        <message>
            <source>1 result found. Use up and down arrow keys to navigate.</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>1 result found. Use up and down arrow keys to navigate.</translation>
        </message>
        <message>
            <source>%1 results found. Use up and down arrow keys to navigate.</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>%1 results found. Use up and down arrow keys to navigate.</translation>
        </message>
        <message>
            <source>Frequently Used</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Frequently Used</translation>
        </message>
        <message>
            <source>Smileys &amp; People</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Smileys &amp; People</translation>
        </message>
        <message>
            <source>Animals &amp; Nature</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Animals &amp; Nature</translation>
        </message>
        <message>
            <source>Food &amp; Drink</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Food &amp; Drink</translation>
        </message>
        <message>
            <source>Travel &amp; Places</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Travel &amp; Places</translation>
        </message>
        <message>
            <source>Activities</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Activities</translation>
        </message>
        <message>
            <source>Objects</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Objects</translation>
        </message>
        <message>
            <source>Symbols</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Symbols</translation>
        </message>
        <message>
            <source>Flags</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Flags</translation>
        </message>
        <message>
            <source>Default skin tone</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Default skin tone</translation>
        </message>
        <message>
            <source>Light skin tone</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Light skin tone</translation>
        </message>
        <message>
            <source>Medium-light skin tone</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Medium-light skin tone</translation>
        </message>
        <message>
            <source>Medium skin tone</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Medium skin tone</translation>
        </message>
        <message>
            <source>Medium-dark skin tone</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Medium-dark skin tone</translation>
        </message>
        <message>
            <source>Dark skin tone</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Dark skin tone</translation>
        </message>
        <message>
            <source>Search</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Search</translation>
        </message>
        <message>
            <source>Search reactions</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Search reactions</translation>
        </message>
        <message>
            <source>Clear search</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Clear search</translation>
        </message>
        <message>
            <source>Choose skin tone</source>
            <translatorcomment>Keep English to match the pinned source picker: the web app does not override emoji-picker-react labels when its UI language changes.</translatorcomment>
            <translation>Choose skin tone</translation>
        </message>
    </context>
    <context>
        <name>workspace::ApiClient</name>
        <message>
            <source>Authentication failed (HTTP 401).</source>
            <translation>Не удалось войти (HTTP 401).</translation>
        </message>
        <message>
            <source>Access denied (HTTP 403).</source>
            <translation>Доступ запрещён (HTTP 403).</translation>
        </message>
        <message>
            <source>Request failed (HTTP %1).</source>
            <translation>Ошибка запроса (HTTP %1).</translation>
        </message>
        <message>
            <source>Enter an HTTPS server origin without a path, credentials, query, or fragment.</source>
            <translation>Введите HTTPS-адрес сервера без пути, учётных данных, параметров и фрагмента.</translation>
        </message>
        <message>
            <source>Invalid API server or path.</source>
            <translation>Неверный адрес или путь API.</translation>
        </message>
        <message>
            <source>Unsupported API method.</source>
            <translation>Неподдерживаемый метод API.</translation>
        </message>
        <message>
            <source>API request body must be a JSON object or array.</source>
            <translation>Тело запроса API должно содержать JSON-объект или массив.</translation>
        </message>
        <message>
            <source>Invalid API request header.</source>
            <translation>Неверный заголовок запроса API.</translation>
        </message>
        <message>
            <source>Sign in to continue.</source>
            <translation>Войдите, чтобы продолжить.</translation>
        </message>
        <message>
            <source>Invalid session credential.</source>
            <translation>Недействительные данные сеанса.</translation>
        </message>
        <message>
            <source>Request cancelled because the session changed.</source>
            <translation>Запрос отменён из-за смены сеанса.</translation>
        </message>
        <message>
            <source>Server response exceeds the allowed size.</source>
            <translation>Ответ сервера превышает допустимый размер.</translation>
        </message>
        <message>
            <source>Request timed out.</source>
            <translation>Время ожидания запроса истекло.</translation>
        </message>
        <message>
            <source>Redirects are not allowed.</source>
            <translation>Перенаправления запрещены.</translation>
        </message>
        <message>
            <source>Network request failed.</source>
            <translation>Не удалось выполнить сетевой запрос.</translation>
        </message>
        <message>
            <source>Server returned invalid JSON.</source>
            <translation>Сервер вернул некорректный JSON.</translation>
        </message>
        <message>
            <source>Server returned an empty response.</source>
            <translation>Сервер вернул пустой ответ.</translation>
        </message>
        <message>
            <source>No request identifiers are available.</source>
            <translation>Нет доступных идентификаторов запроса.</translation>
        </message>
        <message>
            <source>Request cancelled.</source>
            <translation>Запрос отменён.</translation>
        </message>
    </context>
    <context>
        <name>workspace::FileClient</name>
        <message>
            <source>Invalid file identifier.</source>
            <translation>Некорректный идентификатор файла.</translation>
        </message>
        <message>
            <source>File exceeds the allowed in-memory size of 25 MiB.</source>
            <translation>Размер файла превышает допустимые для загрузки в память 25 МиБ.</translation>
        </message>
        <message>
            <source>Redirects are not allowed.</source>
            <translation>Перенаправления запрещены.</translation>
        </message>
        <message>
            <source>Authentication failed (HTTP 401).</source>
            <translation>Не удалось войти (HTTP 401).</translation>
        </message>
        <message>
            <source>Access denied (HTTP 403).</source>
            <translation>Доступ запрещён (HTTP 403).</translation>
        </message>
        <message>
            <source>File request failed (HTTP %1).</source>
            <translation>Ошибка запроса файла (HTTP %1).</translation>
        </message>
        <message>
            <source>File network request failed.</source>
            <translation>Не удалось выполнить сетевой запрос файла.</translation>
        </message>
        <message>
            <source>File transfer timed out.</source>
            <translation>Время ожидания передачи файла истекло.</translation>
        </message>
        <message>
            <source>File transfer cancelled because the session changed.</source>
            <translation>Передача файла отменена из-за смены сеанса.</translation>
        </message>
        <message>
            <source>File transfer cancelled.</source>
            <translation>Передача файла отменена.</translation>
        </message>
        <message>
            <source>Select a nonempty file of at most 25 MiB and a valid stream.</source>
            <translation>Выберите непустой файл размером до 25 МиБ и доступную беседу.</translation>
        </message>
        <message>
            <source>Enter a valid file name.</source>
            <translation>Введите допустимое имя файла.</translation>
        </message>
        <message>
            <source>Cannot read the selected file.</source>
            <translation>Не удалось прочитать выбранный файл.</translation>
        </message>
        <message>
            <source>The image file type does not match its contents.</source>
            <translation>Тип файла изображения не соответствует его содержимому.</translation>
        </message>
        <message>
            <source>Cannot read the complete selected file.</source>
            <translation>Не удалось прочитать выбранный файл целиком.</translation>
        </message>
        <message>
            <source>Cannot rewind the selected file.</source>
            <translation>Не удалось вернуться к началу выбранного файла.</translation>
        </message>
        <message>
            <source>Sign in to upload files.</source>
            <translation>Войдите, чтобы загружать файлы.</translation>
        </message>
        <message>
            <source>Invalid file identifier or destination.</source>
            <translation>Неверный идентификатор файла или путь сохранения.</translation>
        </message>
        <message>
            <source>Sign in to download files.</source>
            <translation>Войдите, чтобы скачивать файлы.</translation>
        </message>
        <message>
            <source>File metadata exceeds the allowed size.</source>
            <translation>Сведения о файле превышают допустимый размер.</translation>
        </message>
        <message>
            <source>Server returned invalid file metadata.</source>
            <translation>Сервер вернул некорректные сведения о файле.</translation>
        </message>
        <message>
            <source>Uploaded file metadata does not match the selected file.</source>
            <translation>Сведения о загруженном файле не соответствуют выбранному файлу.</translation>
        </message>
        <message>
            <source>Server returned a different file identifier.</source>
            <translation>Сервер вернул другой идентификатор файла.</translation>
        </message>
        <message>
            <source>Not enough free space to save the file.</source>
            <translation>Недостаточно свободного места для сохранения файла.</translation>
        </message>
        <message>
            <source>Cannot create the destination file.</source>
            <translation>Не удалось создать файл для сохранения.</translation>
        </message>
        <message>
            <source>Download size does not match file metadata.</source>
            <translation>Размер скачанного файла не соответствует сведениям о нём.</translation>
        </message>
        <message>
            <source>Download exceeds the declared file size.</source>
            <translation>Размер скачиваемых данных превышает заявленный размер файла.</translation>
        </message>
        <message>
            <source>Cannot write the destination file.</source>
            <translation>Не удалось записать файл.</translation>
        </message>
        <message>
            <source>Downloaded file is incomplete.</source>
            <translation>Файл скачан не полностью.</translation>
        </message>
        <message>
            <source>Downloaded file checksum does not match.</source>
            <translation>Контрольная сумма скачанного файла не совпадает.</translation>
        </message>
        <message>
            <source>Cannot commit the destination file.</source>
            <translation>Не удалось завершить сохранение файла.</translation>
        </message>
    </context>
    <context>
        <name>workspace::NativeCache</name>
        <message>
            <source>The cache limit must be between 1 MiB and 1000 GB.</source>
            <translation>Лимит кэша должен быть от 1 МиБ до 1000 ГБ.</translation>
        </message>
        <message>
            <source>The local message cache is unavailable.</source>
            <translation>Локальный кэш сообщений недоступен.</translation>
        </message>
    </context>
    <context>
        <name>workspace::NativeCacheService</name>
        <message>
            <source>The cache limit must be between 1 MiB and 1000 GB.</source>
            <translation>Лимит кэша должен быть от 1 МиБ до 1000 ГБ.</translation>
        </message>
        <message>
            <source>The local message cache is unavailable.</source>
            <translation>Локальный кэш сообщений недоступен.</translation>
        </message>
        <message>
            <source>The local cache cleanup queue is busy. Please try again.</source>
            <translation>Очередь очистки кэша занята. Повторите попытку.</translation>
        </message>
    </context>
    <context>
        <name>workspace::NativeController</name>
        <message>
            <source>Could not update key sharing.</source>
            <translation>Не удалось изменить настройку передачи ключей.</translation>
        </message>
        <message><source>Retry</source><translation>Повторить</translation></message>
        <message><source>Could not mark the topic as read. Please try again.</source><translation>Не удалось отметить топик как прочитанный. Попробуйте ещё раз.</translation></message>
        <message><source>Could not rename the topic. Please try again.</source><translation>Не удалось переименовать топик. Попробуйте ещё раз.</translation></message>
        <message><source>Could not update the topic notifications. Please try again.</source><translation>Не удалось обновить уведомления топика. Попробуйте ещё раз.</translation></message>
        <message><source>Could not update the topic. Please try again.</source><translation>Не удалось обновить топик. Попробуйте ещё раз.</translation></message>
        <message><source>The topic rename was interrupted by a session refresh. Try again.</source><translation>Переименование топика прервано обновлением сессии. Повторите попытку.</translation></message>
        <message><source>The topic notification change was interrupted by a session refresh. Try again.</source><translation>Изменение уведомлений топика прервано обновлением сессии. Повторите попытку.</translation></message>
        <message><source>The topic read update was interrupted by a session refresh. Try again.</source><translation>Отметка топика прочитанным прервана обновлением сессии. Повторите попытку.</translation></message>
        <message><source>The topic done update was interrupted by a session refresh. Try again.</source><translation>Обновление статуса топика прервано обновлением сессии. Повторите попытку.</translation></message>
    <message>
        <source>Could not send the key. Requests to retry: %1.</source>
        <translation>Не удалось отправить ключ. Запросов для повторной попытки: %1.</translation>
    </message>
    <message>
        <source>The request could not be rejected.</source>
        <translation>Не удалось отклонить запрос.</translation>
    </message>
    <message>
        <source>This request has not been sent.</source>
        <translation>Этот запрос ещё не отправлен.</translation>
    </message>
    <message>
        <source>The key could not be sent to this device.</source>
        <translation>Не удалось отправить ключ на это устройство.</translation>
    </message>

        <message>
            <source>Show spoiler</source>
            <translation>Показать спойлер</translation>
        </message>
        <message>
            <source>Hide spoiler</source>
            <translation>Скрыть спойлер</translation>
        </message>
        <message>
            <source>Request key</source>
            <translation>Запросить ключ</translation>
        </message>
        <message>
            <source>Waiting for a response</source>
            <translation>Ожидание ответа</translation>
        </message>
        <message>
            <source>Could not decrypt message</source>
            <translation>Не удалось расшифровать</translation>
        </message>
        <message>
            <source>Some local data could not be cleared.</source>
            <translation>Не удалось очистить часть локальных данных.</translation>
        </message>
        <message><source>Please fill in all fields</source><translation>Заполните все поля</translation></message>
    <message><source>Could not open Favorites.</source><translation>Не удалось открыть Избранное.</translation></message>
    <message>
        <source>The authentication code is invalid. Try again.</source>
        <translation>Код подтверждения неверный. Попробуйте ещё раз.</translation>
    </message>
        <message><source>The selected file changed. Choose it again.</source><translation>Выбранный файл изменился. Выберите его снова.</translation></message>
        <message><source>Use the file picker to attach clipboard files on this platform.</source><translation>Чтобы прикрепить файлы из буфера обмена на этой платформе, выберите их через диалог файлов.</translation></message>
        <message><source>Paste up to 100 files totalling at most 100 MiB.</source><translation>Вставьте не более 100 файлов общим размером до 100 МиБ.</translation></message>
        <message>
            <source>Link</source>
            <translation>Ссылка</translation>
        </message>
        <message>
            <source>Image</source>
            <translation>Изображение</translation>
        </message>
        <message>
            <source>Video</source>
            <translation>Видео</translation>
        </message>
        <message>
            <source>File</source>
            <translation>Файл</translation>
        </message>
        <message>
            <source>File: %1</source>
            <translation>Файл: %1</translation>
        </message>
        <message>
            <source>Quote: %1</source>
            <translation>Цитата: %1</translation>
        </message>
        <message>
            <source>Forwarded message</source>
            <translation>Пересланное сообщение</translation>
        </message>
        <message>
            <source>Encrypted fragment</source>
            <translation>Зашифрованный фрагмент</translation>
        </message>
        <message>
            <source>Upload failed</source>
            <translation>Ошибка загрузки</translation>
        </message>
        <message>
            <source>The file is empty</source>
            <translation>Файл пуст</translation>
        </message>
        <message>
            <source>The file is too large (maximum 25 MB)</source>
            <translation>Файл слишком большой (не более 25 МБ)</translation>
        </message>
        <message>
            <source>The image file type does not match its contents</source>
            <translation>Тип изображения не соответствует его содержимому</translation>
        </message>
        <message>
            <source>The file could not be validated</source>
            <translation>Не удалось проверить файл</translation>
        </message>
        <message>
            <source>The attachment download could not be started.</source>
            <translation>Не удалось начать загрузку вложения.</translation>
        </message>
        <message>
            <source>The downloaded file is missing.</source>
            <translation>Загруженный файл не найден.</translation>
        </message>
        <message>
            <source>The downloaded file could not be opened.</source>
            <translation>Не удалось открыть загруженный файл.</translation>
        </message>
        <message>
            <source>The download folder could not be opened.</source>
            <translation>Не удалось открыть папку загрузки.</translation>
        </message>
        <message>
            <source>All chats</source>
            <translation>Все</translation>
        </message>
        <message>
            <source>Personal</source>
            <translation>Личные</translation>
        </message>
        <message>
            <source>Channels</source>
            <translation>Каналы</translation>
        </message>
        <message>
            <source>Call could not be started.</source>
            <translation>Не удалось начать звонок.</translation>
        </message>
        <message>
            <source>Encryption is unavailable for this chat or local key storage could not be opened.</source>
            <translation>Шифрование для этого чата недоступно или не удалось открыть локальное хранилище ключей.</translation>
        </message>
        <message>
            <source>Calls cannot start in encrypted channels yet.</source>
            <translation>В защищённых каналах звонки пока недоступны.</translation>
        </message>
        <message>
            <source>Server address must be a valid HTTPS URL</source>
            <translation>Адрес сервера должен быть валидным HTTPS URL</translation>
        </message>
        <message>
            <source>The saved credential could not be removed. %1</source>
            <translation>Не удалось удалить сохранённые учётные данные. %1</translation>
        </message>
        <message>
            <source>Could not load organization settings. Check the server address and try again.</source>
            <translation>Не удалось загрузить настройки организации. Проверьте адрес сервера и попробуйте ещё раз.</translation>
        </message>
        <message>
            <source>Sign in to an account for the organization and project in this link.</source>
            <translation>Войдите в аккаунт организации и проекта, указанных в ссылке.</translation>
        </message>
        <message>
            <source>Exordos Core Public</source>
            <translation>Exordos Core Public</translation>
        </message>
        <message>
            <source>All messages</source>
            <translation>Все сообщения</translation>
        </message>
        <message>
            <source>Drafts could not be synchronized. Refresh to retry.</source>
            <translation>Не удалось синхронизировать черновики. Обновите список для повторной попытки.</translation>
        </message>
        <message>
            <source>Yesterday</source>
            <translation>Вчера</translation>
        </message>
        <message>
            <source>The draft could not be saved on this computer. Your input is still in memory. Retry before quitting.</source>
            <translation>Не удалось сохранить черновик на этом компьютере. Введённый текст остаётся в памяти. Повторите попытку перед выходом.</translation>
        </message>
        <message>
            <source>Failed to update status</source>
            <translation>Не удалось обновить статус</translation>
        </message>
        <message>
            <source>Could not create the topic. %1</source>
            <translation>Не удалось создать тему. %1</translation>
        </message>
        <message>
            <source>Enter a topic name with 1 to 128 characters.</source>
            <translation>Введите название темы длиной от 1 до 128 символов.</translation>
        </message>
        <message>
            <source>Resolve the earlier topic creation attempts before creating another topic.</source>
            <translation>Сначала выясните результат предыдущих попыток создания темы.</translation>
        </message>
        <message>
            <source>The created topic was deleted.</source>
            <translation>Созданная тема была удалена.</translation>
        </message>
        <message>
            <source>The topic creation status is unknown. Check status before trying again.</source>
            <translation>Результат создания темы неизвестен. Проверьте статус перед новой попыткой.</translation>
        </message>
        <message>
            <source>The topic was created, but the channel list could not be updated. Check status.</source>
            <translation>Тема создана, но список тем канала не обновлён. Проверьте статус.</translation>
        </message>
        <message>
            <source>The topic was created. Check status to open it.</source>
            <translation>Тема создана. Проверьте статус, чтобы открыть её.</translation>
        </message>
        <message>
            <source>Wait for the channel list to update before creating a topic.</source>
            <translation>Перед созданием темы дождитесь обновления списка каналов.</translation>
        </message>
        <message>
            <source>Signed out, but the local message cache could not be cleared.</source>
            <translation>Вы вышли из аккаунта, но очистить локальный кэш сообщений не удалось.</translation>
        </message>
        <message>
            <source>The local cache could not be cleared. The saved account was kept.</source>
            <translation>Не удалось очистить локальный кэш. Сохранённый аккаунт не удалён.</translation>
        </message>
        <message>
            <source>The channel creation status is unknown. Check status before trying again.</source>
            <translation>Результат создания канала неизвестен. Проверьте его перед повторной попыткой.</translation>
        </message>
        <message>
            <source>The created channel was deleted.</source>
            <translation>Созданный канал удалён.</translation>
        </message>
        <message>
            <source>The created default topic was deleted.</source>
            <translation>Тема по умолчанию созданного канала удалена.</translation>
        </message>
        <message>
            <source>Enter a channel name with 1 to 255 characters.</source>
            <translation>Введите название канала длиной от 1 до 255 символов.</translation>
        </message>
        <message>
            <source>Enter a description with no more than 10000 characters.</source>
            <translation>Описание должно содержать не более 10000 символов.</translation>
        </message>
        <message>
            <source>Wait for the channel list to update before creating a channel.</source>
            <translation>Дождитесь обновления списка каналов перед созданием канала.</translation>
        </message>
        <message>
            <source>The selected channel members are invalid. Choose members from the current directory.</source>
            <translation>Не удалось подтвердить выбранных участников. Выберите их из текущего списка пользователей.</translation>
        </message>
        <message>
            <source>Wait for the member directory to update before creating this channel.</source>
            <translation>Дождитесь обновления списка пользователей перед созданием этого канала.</translation>
        </message>
        <message>
            <source>Encrypted channel creation is not available yet.</source>
            <translation>Создание зашифрованных каналов пока недоступно.</translation>
        </message>
        <message>
            <source>The server did not confirm encrypted channel creation. Check status before trying again.</source>
            <translation>Сервер не подтвердил создание зашифрованного канала. Перед повторной попыткой проверьте состояние.</translation>
        </message>
        <message>
            <source>Could not store the encrypted channel key. Check status before trying again.</source>
            <translation>Не удалось сохранить ключ зашифрованного канала. Перед повторной попыткой проверьте состояние.</translation>
        </message>
        <message>
            <source>Resolve the earlier channel creation attempts before creating another channel.</source>
            <translation>Сначала уточните результат предыдущих попыток создания канала.</translation>
        </message>
        <message>
            <source>All encrypted channel creation slots are held by unresolved attempts. Resolve one of them and try again.</source>
            <translation>Все слоты создания зашифрованных каналов заняты не уточнёнными попытками. Уточните одну из них и повторите попытку.</translation>
        </message>
        <message>
            <source>The channel creation status check was interrupted by a session change. The attempt was kept; check status again.</source>
            <translation>Проверка состояния создания канала прервана из-за смены сессии. Попытка сохранена; проверьте состояние ещё раз.</translation>
        </message>
        <message>
            <source>The channel creation attempt identity is invalid.</source>
            <translation>Идентификатор попытки создания канала некорректен.</translation>
        </message>
        <message>
            <source>This creation attempt is no longer unresolved.</source>
            <translation>Эта попытка создания канала больше не требует уточнения.</translation>
        </message>
        <message>
            <source>This creation attempt is still in progress. Check its status later.</source>
            <translation>Эта попытка создания канала ещё выполняется. Проверьте её состояние позже.</translation>
        </message>
        <message>
            <source>The creation attempt could not be resolved right now.</source>
            <translation>Не удалось уточнить результат попытки создания канала.</translation>
        </message>
        <message>
            <source>The creation attempt never reached the server and was retired.</source>
            <translation>Попытка создания канала не была отправлена на сервер и удалена.</translation>
        </message>
        <message>
            <source>The unresolved channel was found and its encryption key was saved.</source>
            <translation>Не уточнённый канал был найден, и его ключ шифрования сохранён.</translation>
        </message>
        <message>
            <source>The channel was not found yet. The attempt and its encryption key were kept.</source>
            <translation>Канал пока не найден. Попытка и её ключ шифрования сохранены.</translation>
        </message>
        <message>
            <source>The attempt was kept: discarding requires an explicit confirmation, because a channel that appears later could never be read.</source>
            <translation>Попытка сохранена: удаление требует явного подтверждения, поскольку канал, который появится позже, нельзя будет прочитать.</translation>
        </message>
        <message>
            <source>A check for this attempt is already running. Try again when it finishes.</source>
            <translation>Для этой попытки уже выполняется проверка. Повторите попытку после её завершения.</translation>
        </message>
        <message>
            <source>The creation attempt was discarded. If its channel appears later, it can never be read.</source>
            <translation>Попытка создания канала удалена. Если её канал появится позже, его нельзя будет прочитать.</translation>
        </message>
        <message>
            <source>The channel exists but did not confirm this key. The attempt was kept.</source>
            <translation>Канал существует, но не подтвердил этот ключ. Попытка сохранена.</translation>
        </message>
        <message>
            <source>The server did not answer conclusively. The attempt was kept.</source>
            <translation>Сервер не дал определённого ответа. Попытка сохранена.</translation>
        </message>
        <message>
            <source>Could not create the channel. %1</source>
            <translation>Не удалось создать канал. %1</translation>
        </message>
        <message>
            <source>The channel was created, but its status could not be verified. Check status.</source>
            <translation>Канал создан, но проверить его состояние не удалось. Проверьте результат.</translation>
        </message>
        <message>
            <source>The server returned an invalid channel creation response.</source>
            <translation>Сервер вернул некорректный ответ на создание канала.</translation>
        </message>
        <message>
            <source>The chat status is unknown. Check status before trying again.</source>
            <translation>Результат открытия чата неизвестен. Проверьте его перед повторной попыткой.</translation>
        </message>
        <message>
            <source>The chat changed or is no longer accessible. Check status.</source>
            <translation>Чат изменён или больше недоступен. Проверьте результат.</translation>
        </message>
        <message>
            <source>The saved conversation is no longer available.</source>
            <translation>Сохранённый чат больше недоступен.</translation>
        </message>
        <message>
            <source>Too many pending chat accounts. Restart the application before opening another chat.</source>
            <translation>Достигнут лимит учётных записей для открытия чатов. Перезапустите приложение.</translation>
        </message>
        <message>
            <source>The chat changed. Check status to open it.</source>
            <translation>Чат изменён. Проверьте результат, чтобы открыть его.</translation>
        </message>
        <message>
            <source>The chat could not yet be found. Check status, or explicitly retry Start chat.</source>
            <translation>Чат пока не найден. Проверьте результат или нажмите «Повторить».</translation>
        </message>
        <message>
            <source>The server did not verify this personal chat.</source>
            <translation>Не удалось подтвердить данные личного чата.</translation>
        </message>
        <message>
            <source>This personal chat is archived. Opening archived chats is not supported yet.</source>
            <translation>Этот личный чат в архиве. Открытие архивных чатов пока не поддерживается.</translation>
        </message>
        <message>
            <source>The server did not return a valid default topic for this chat.</source>
            <translation>Сервер не вернул корректную тему по умолчанию для этого чата.</translation>
        </message>
        <message>
            <source>Choose a person from the updated member directory.</source>
            <translation>Выберите пользователя из обновлённого списка.</translation>
        </message>
        <message>
            <source>Could not start the personal chat. %1</source>
            <translation>Не удалось открыть личный чат. %1</translation>
        </message>
        <message>
            <source>The server returned an invalid personal chat list.</source>
            <translation>Сервер вернул некорректный список личных чатов.</translation>
        </message>
        <message>
            <source>The chat could not be verified. Check status.</source>
            <translation>Не удалось уточнить состояние чата. Проверьте результат.</translation>
        </message>
        <message>
            <source>The chat is missing from the current chat list. Check status.</source>
            <translation>Чат отсутствует в текущем списке. Проверьте результат.</translation>
        </message>
        <message>
            <source>The chat is ready. Check status to open it.</source>
            <translation>Чат готов. Проверьте результат, чтобы открыть его.</translation>
        </message>
        <message>
            <source>The server returned an invalid default topic.</source>
            <translation>Сервер вернул некорректную тему по умолчанию.</translation>
        </message>
        <message>
            <source>The channel was created. Add the selected members to finish.</source>
            <translation>Канал создан. Добавьте выбранных участников, чтобы завершить создание.</translation>
        </message>
        <message>
            <source>The server returned invalid channel memberships.</source>
            <translation>Сервер вернул некорректные данные участников канала.</translation>
        </message>
        <message>
            <source>The channel was created, but members could not be added. %1</source>
            <translation>Канал создан, но добавить участников не удалось. %1</translation>
        </message>
        <message>
            <source>The channel membership status is unknown. Check status before retrying members.</source>
            <translation>Результат добавления участников неизвестен. Проверьте его перед повторной попыткой.</translation>
        </message>
        <message>
            <source>The channel was created, but the channel list changed. Check status.</source>
            <translation>Канал создан, но список каналов изменился. Проверьте результат.</translation>
        </message>
        <message>
            <source>The channel was created, but it is missing from the channel list. Check status.</source>
            <translation>Канал создан, но отсутствует в списке каналов. Проверьте результат.</translation>
        </message>
        <message>
            <source>The channel was created. Check status to open it.</source>
            <translation>Канал создан. Проверьте результат, чтобы открыть его.</translation>
        </message>
        <message>
            <source>Could not verify ownership of this reaction.</source>
            <translation>Не удалось проверить, принадлежит ли вам эта реакция.</translation>
        </message>
        <message>
            <source>The reaction could not be changed. %1</source>
            <translation>Не удалось изменить реакцию. %1</translation>
        </message>
        <message>
            <source>The reaction response could not be verified.</source>
            <translation>Не удалось проверить ответ на реакцию.</translation>
        </message>
        <message>
            <source>The reaction has not been confirmed. Retry checks its status without sending it again.</source>
            <translation>Реакция не подтверждена. Повтор проверит её статус без повторной отправки.</translation>
        </message>
        <message>
            <source>Could not refresh reaction metadata.</source>
            <translation>Не удалось обновить сведения о реакции.</translation>
        </message>
        <message>
            <source>Attachment transfer interrupted by a session change. Choose the file again.</source>
            <translation>Передача вложения прервана из-за смены сессии. Выберите файл снова.</translation>
        </message>
        <message>
            <source>Attachment service changed. Choose the file again.</source>
            <translation>Сервис вложений изменился. Выберите файл снова.</translation>
        </message>
        <message>
            <source>Attachment service is unavailable. Choose the file again.</source>
            <translation>Сервис вложений недоступен. Выберите файл снова.</translation>
        </message>
        <message>
            <source>Choose a local file to attach.</source>
            <translation>Выберите локальный файл для вложения.</translation>
        </message>
        <message>
            <source>Uploading attachment…</source>
            <translation>Отправка вложения…</translation>
        </message>
        <message>
            <source>The attachment could not be uploaded. %1</source>
            <translation>Не удалось отправить вложение. %1</translation>
        </message>
        <message>
            <source>Message is too long (maximum 40,000 characters).</source>
            <translation>Сообщение слишком длинное (не более 40 000 символов).</translation>
        </message>
        <message>
            <source>Uploading attachments…</source>
            <translation>Отправка вложений…</translation>
        </message>
        <message>
            <source>Attachment is ready to send.</source>
            <translation>Вложение готово к отправке.</translation>
        </message>
        <message>
            <source>Downloading attachment…</source>
            <translation>Скачивание вложения…</translation>
        </message>
        <message>
            <source>The requested message is unavailable.</source>
            <translation>Запрошенное сообщение недоступно.</translation>
        </message>
        <message>
            <source>The attachment could not be saved. %1</source>
            <translation>Не удалось сохранить вложение. %1</translation>
        </message>
        <message>
            <source>Saved %1.</source>
            <translation>Сохранено: %1.</translation>
        </message>
        <message>
            <source>A live update could not be applied. Reloading the current state.</source>
            <translation>Не удалось применить обновление в реальном времени. Загружается текущее состояние.</translation>
        </message>
        <message>
            <source>Encrypted message — key support is under development</source>
            <translation>Зашифрованное сообщение — поддержка ключей в разработке</translation>
        </message>
        <message>
            <source>System message (%1)</source>
            <translation>Системное сообщение (%1)</translation>
        </message>
        <message>
            <source>You</source>
            <translation>Вы</translation>
        </message>
        <message>
            <source>Workspace</source>
            <translation>Workspace</translation>
        </message>
        <message>
            <source>Could not load the catalog. %1</source>
            <translation>Не удалось загрузить каталог. %1</translation>
        </message>
        <message>
            <source>Loading…</source>
            <translation>Загрузка…</translation>
        </message>
        <message>
            <source>Some sidebar data could not be refreshed</source>
            <translation>Часть данных боковой панели не обновилась</translation>
        </message>
        <message>
            <source>No matches</source>
            <translation>Ничего не найдено</translation>
        </message>
        <message>
            <source>Try a different chat or topic name.</source>
            <translation>Попробуйте другое название чата или темы.</translation>
        </message>
        <message>
            <source>No workspace chats yet</source>
            <translation>В Workspace пока нет чатов</translation>
        </message>
        <message>
            <source>When Workspace conversations load, they will show up here.</source>
            <translation>Когда чаты Workspace загрузятся, они появятся здесь.</translation>
        </message>
        <message>
            <source>No direct messages yet</source>
            <translation>Пока нет личных сообщений</translation>
        </message>
        <message>
            <source>Start a DM from the compose menu or directory.</source>
            <translation>Начните личный диалог через меню создания чата или каталог.</translation>
        </message>
        <message>
            <source>No channels yet</source>
            <translation>Пока нет каналов</translation>
        </message>
        <message>
            <source>Subscribe to streams you care about to see them here.</source>
            <translation>Подпишитесь на интересующие каналы, чтобы они отображались здесь.</translation>
        </message>
        <message>
            <source>Folder is empty</source>
            <translation>Папка пуста</translation>
        </message>
        <message>
            <source>Add chats from a chat menu to populate this folder.</source>
            <translation>Добавьте чаты через меню чата, чтобы заполнить папку.</translation>
        </message>
        <message>
            <source>The server returned an invalid pagination cursor.</source>
            <translation>Сервер вернул недопустимый курсор пагинации.</translation>
        </message>
        <message>
            <source>The server returned an invalid channel list.</source>
            <translation>Сервер вернул недопустимый список каналов.</translation>
        </message>
        <message>
            <source>The server returned an invalid topic list.</source>
            <translation>Сервер вернул недопустимый список тем.</translation>
        </message>
        <message>
            <source>Could not load messages. %1</source>
            <translation>Не удалось загрузить сообщения. %1</translation>
        </message>
        <message>
            <source>The server returned an invalid conversation history.</source>
            <translation>Сервер вернул недопустимую историю беседы.</translation>
        </message>
        <message>
            <source>The server returned an invalid history cursor.</source>
            <translation>Сервер вернул недопустимый курсор истории.</translation>
        </message>
        <message>
            <source>Sending was not confirmed. Your text is preserved. %1</source>
            <translation>Отправка не подтверждена. Текст сохранён. %1</translation>
        </message>
        <message>
            <source>Message verification failed. Your text is preserved.</source>
            <translation>Не удалось проверить сообщение. Текст сохранён.</translation>
        </message>
        <message>
            <source>The read marker could not be confirmed. Refresh the conversation to try again.</source>
            <translation>Не удалось подтвердить метку прочтения. Обновите беседу, чтобы повторить попытку.</translation>
        </message>
        <message>
            <source>The read marker was saved, but unread counters could not be refreshed.</source>
            <translation>Метка прочтения сохранена, но счётчики непрочитанных не удалось обновить.</translation>
        </message>
        <message>
            <source>The message change could not be verified.</source>
            <translation>Не удалось проверить изменение сообщения.</translation>
        </message>
        <message>
            <source>Could not change starred state. %1</source>
            <translation>Не удалось изменить состояние «в избранном». %1</translation>
        </message>
        <message>
            <source>Could not edit the message. %1</source>
            <translation>Не удалось изменить сообщение. %1</translation>
        </message>
        <message>
            <source>This message can no longer be edited</source>
            <translation>Это сообщение уже нельзя редактировать</translation>
        </message>
        <message>
            <source>This message can no longer be replied to.</source>
            <translation>Это сообщение уже нельзя открыть для ответа.</translation>
        </message>
        <message>
            <source>Quote</source>
            <translation>Цитата</translation>
        </message>
        <message>
            <source>Could not confirm whether the topic reopened. Refresh before trying again.</source>
            <translation>Не удалось подтвердить повторное открытие темы. Обновите данные перед следующей попыткой.</translation>
        </message>
        <message>
            <source>Could not reopen the topic. %1</source>
            <translation>Не удалось открыть тему повторно. %1</translation>
        </message>
        <message>
            <source>Could not delete the message. %1</source>
            <translation>Не удалось удалить сообщение. %1</translation>
        </message>
        <message>
            <source>Could not unarchive the chat. Try again.</source>
            <translation>Не удалось разархивировать чат. Попробуйте ещё раз.</translation>
        </message>
        <message>
            <source>Channel access has been revoked.</source>
            <translation>Доступ к каналу отозван.</translation>
        </message>
        <message>
            <source>Encryption authority exceeds the request limit.</source>
            <translation>Превышен лимит запросов при проверке прав на шифрование.</translation>
        </message>
        <message>
            <source>Could not verify encryption authority.</source>
            <translation>Не удалось проверить права на шифрование.</translation>
        </message>
        <message>
            <source>Encryption authority exceeds the response limit.</source>
            <translation>Превышен допустимый объём ответа при проверке прав на шифрование.</translation>
        </message>
        <message>
            <source>Encryption authority pagination repeated.</source>
            <translation>При проверке прав на шифрование повторилась страница результатов.</translation>
        </message>
        <message>
            <source>A verified live channel is required to request a key.</source>
            <translation>Для запроса ключа нужен канал, проверенный через активное подключение.</translation>
        </message>
        <message>
            <source>This channel's encryption authority is unavailable.</source>
            <translation>Не удалось подтвердить права на шифрование в этом канале.</translation>
        </message>
        <message>
            <source>The encryption request topic could not be verified.</source>
            <translation>Не удалось проверить тему для запроса ключа шифрования.</translation>
        </message>
        <message>
            <source>The default topic changed.</source>
            <translation>Тема по умолчанию изменилась.</translation>
        </message>
        <message>
            <source>The encryption participants could not be verified.</source>
            <translation>Не удалось проверить участников шифрованного канала.</translation>
        </message>
        <message>
            <source>Current channel access could not be verified.</source>
            <translation>Не удалось проверить текущие права доступа к каналу.</translation>
        </message>
        <message>
            <source>The device name exceeds 255 Unicode characters.</source>
            <translation>Имя устройства превышает 255 символов Unicode.</translation>
        </message>
        <message>
            <source>The saved key request could not be verified.</source>
            <translation>Не удалось проверить сохранённый запрос ключа.</translation>
        </message>
        <message>
            <source>There is no saved request to check.</source>
            <translation>Нет сохранённого запроса для проверки.</translation>
        </message>
        <message>
            <source>A secure request key pair could not be created.</source>
            <translation>Не удалось создать безопасную пару ключей для запроса.</translation>
        </message>
        <message>
            <source>The key request could not be saved securely.</source>
            <translation>Не удалось безопасно сохранить запрос ключа.</translation>
        </message>
        <message>
            <source>This legacy request needs history reconciliation.</source>
            <translation>Для этого старого запроса нужна сверка с историей сообщений.</translation>
        </message>
        <message>
            <source>The key request has not been confirmed. Check does not send it again.</source>
            <translation>Запрос ключа не подтверждён. Проверка не отправляет его повторно.</translation>
        </message>
        <message>
            <source>The key request was superseded by a local change.</source>
            <translation>Запрос ключа потерял актуальность из-за локального изменения.</translation>
        </message>
        <message>
            <source>The key response was superseded by a local change.</source>
            <translation>Ответ на запрос ключа потерял актуальность из-за локального изменения.</translation>
        </message>
        <message>
            <source>Enter a valid topic identifier.</source>
            <translation>Введите допустимый идентификатор темы.</translation>
        </message>
        <message>
            <source>The server returned invalid channel metadata.</source>
            <translation>Сервер вернул недопустимые данные канала.</translation>
        </message>
        <message>
            <source>The server returned invalid topic metadata.</source>
            <translation>Сервер вернул недопустимые данные темы.</translation>
        </message>
        <message>
            <source>Sending is unavailable. Check the saved message status.</source>
            <translation>Отправка недоступна. Проверьте состояние сообщения.</translation>
        </message>
        <message>
            <source>Sending was interrupted. Check the saved message status before retrying.</source>
            <translation>Отправка была прервана. Проверьте состояние сообщения перед повтором.</translation>
        </message>
        <message>
            <source>Sending was not confirmed. Check the saved message status.</source>
            <translation>Отправка не подтверждена. Проверьте состояние сообщения.</translation>
        </message>
        <message>
            <source>The encrypted message exceeds the server size limit. Your text is preserved.</source>
            <translation>Зашифрованное сообщение превышает допустимый размер. Ваш текст сохранён.</translation>
        </message>
        <message>
            <source>The message could not be encrypted. Your text is preserved.</source>
            <translation>Не удалось зашифровать сообщение. Ваш текст сохранён.</translation>
        </message>
        <message>
            <source>The saved message status is unresolved. Your text is preserved.</source>
            <translation>Состояние сообщения не определено. Ваш текст сохранён.</translation>
        </message>
        <message>
            <source>The sending key is unavailable. Your text is preserved.</source>
            <translation>Ключ для отправки недоступен. Ваш текст сохранён.</translation>
        </message>
        <message>
        <source>Channel encryption changed. Check the saved key response.</source>
        <translation>Настройки шифрования канала изменились. Проверьте состояние сохранённого ответа на запрос ключа.</translation>
    </message>
    <message>
        <source>Permission to retry the key response is unavailable.</source>
        <translation>Нет разрешения на повторную отправку ответа на запрос ключа.</translation>
    </message>
    <message>
        <source>The default topic changed. Check the saved key response.</source>
        <translation>Тема по умолчанию изменилась. Проверьте состояние сохранённого ответа на запрос ключа.</translation>
    </message>
    <message>
        <source>The key response was interrupted. Check its saved status before retrying.</source>
        <translation>Отправка ответа на запрос ключа прервана. Перед повторной отправкой проверьте его состояние.</translation>
    </message>
    <message>
        <source>The key response was not confirmed. Check does not send it again.</source>
        <translation>Ответ на запрос ключа не подтверждён. Проверка не отправляет его повторно.</translation>
    </message>
    <message>
        <source>The original key request could not be verified.</source>
        <translation>Не удалось проверить исходный запрос ключа.</translation>
    </message>
    <message>
        <source>The requested key could not be encrypted for this device.</source>
        <translation>Не удалось зашифровать запрошенный ключ для этого устройства.</translation>
    </message>
    <message>
        <source>The requested key is no longer available.</source>
        <translation>Запрошенный ключ больше недоступен.</translation>
    </message>
    <message>
        <source>The saved key response is unresolved. Check does not send it again.</source>
        <translation>Состояние сохранённого ответа на запрос ключа не определено. Проверка не отправляет его повторно.</translation>
    </message>
    <message>
        <source>Something went wrong</source>
        <translation>Что-то пошло не так</translation>
    </message>
        <message>
            <source>Could not load profile. Try again.</source>
            <translation>Не удалось загрузить профиль. Попробуйте ещё раз.</translation>
        </message>
        <message>
            <source>Failed to update profile</source>
            <translation>Не удалось обновить профиль</translation>
        </message>
        <message>
            <source>Profile editing is not available yet</source>
            <translation>Редактирование профиля пока недоступно</translation>
        </message>
        <message>
            <source>Your server does not allow this profile request.</source>
            <translation>Сервер не разрешает этот запрос к профилю.</translation>
        </message>
        <message>
            <source>Check the lengths of the editable fields.</source>
            <translation>Проверьте длину редактируемых полей.</translation>
        </message>
        <message>
            <source>Temporarily not connected</source>
            <translation>Временно не подключено</translation>
        </message>
        <message>
            <source>Failed to update avatar</source>
            <translation>Не удалось обновить аватар</translation>
        </message>
        <message>
            <source>Failed to remove avatar</source>
            <translation>Не удалось удалить аватар</translation>
        </message>
        <message>
            <source>Avatar update is not supported by this server</source>
            <translation>Обновление аватара не поддерживается этим сервером</translation>
        </message>
        <message>
            <source>Avatar changes are disabled by server settings</source>
            <translation>Изменение аватара запрещено настройками сервера</translation>
        </message>
        <message>
            <source>Please choose a valid image file</source>
            <translation>Выберите корректный файл изображения</translation>
        </message>
        <message>
            <source>Image is too large (max %1 MB)</source>
            <translation>Изображение слишком большое (макс. %1 МБ)</translation>
        </message>
        <message>
            <source>Could not delete the key from this device. Please try again.</source>
            <translation>Не удалось удалить ключ с этого устройства. Попробуйте ещё раз.</translation>
        </message>
        <message>
            <source>Could not load keys from this device.</source>
            <translation>Не удалось загрузить ключи с этого устройства.</translation>
        </message>
        <message>
            <source>Could not reissue the encryption key. Please try again.</source>
            <translation>Не удалось сменить ключ шифрования. Попробуйте ещё раз.</translation>
        </message>
        <message>
            <source>This key is still being announced. It cannot be deleted yet.</source>
            <translation>Ключ ещё объявляется в чате. Пока его удалить нельзя.</translation>
        </message>
        <message>
            <source>All rotation announcement slots are held by unresolved rotations. Resolve one of them and try again.</source>
            <translation>Все слоты объявлений о ротации заняты неразрешёнными ротациями. Разрешите одну из них и повторите попытку.</translation>
        </message>
        <message>
            <source>The rotation stage identity is invalid.</source>
            <translation>Недопустимый идентификатор этапа ротации.</translation>
        </message>
        <message>
            <source>Encrypted chat keys are not available right now.</source>
            <translation>Ключи шифрования чата сейчас недоступны.</translation>
        </message>
        <message>
            <source>This rotation announcement is still in progress. Check its status later.</source>
            <translation>Объявление этой ротации ещё выполняется. Проверьте статус позже.</translation>
        </message>
        <message>
            <source>This rotation stage is no longer unresolved.</source>
            <translation>Этот этап ротации больше не является неразрешённым.</translation>
        </message>
        <message>
            <source>The rotation announcement never reached the server and was retired.</source>
            <translation>Объявление о ротации так и не было отправлено на сервер и снято.</translation>
        </message>
        <message>
            <source>The rotation stage could not be resolved right now.</source>
            <translation>Не удалось разрешить этап ротации сейчас.</translation>
        </message>
        <message>
            <source>The rotation announcement landed. The staged key is no longer pending.</source>
            <translation>Объявление о ротации доставлено. Сохранённый ключ больше не ожидает.</translation>
        </message>
        <message>
            <source>The rotation announcement was not found yet. The staged key was kept.</source>
            <translation>Объявление о ротации пока не найдено. Сохранённый ключ оставлен на устройстве.</translation>
        </message>
        <message>
            <source>The rotation announcement exists but did not confirm this key. The stage was kept.</source>
            <translation>Объявление о ротации существует, но не подтверждает этот ключ. Этап сохранён.</translation>
        </message>
        <message>
            <source>The server did not answer conclusively. The staged key was kept.</source>
            <translation>Сервер ответил неоднозначно. Сохранённый ключ оставлен на устройстве.</translation>
        </message>
        <message>
            <source>The rotation stage was kept: discarding requires an explicit confirmation, because an announcement that lands later could leave that rotation permanently unreadable.</source>
            <translation>Этап ротации сохранён: отмена требует явного подтверждения, потому что доставленное позже объявление может сделать эту ротацию навсегда нечитаемой.</translation>
        </message>
        <message>
            <source>A check for this rotation is already running. Try again when it finishes.</source>
            <translation>Проверка этой ротации уже выполняется. Повторите попытку, когда она завершится.</translation>
        </message>
        <message>
            <source>The rotation stage was discarded. If its announcement lands later, this device may never read that rotation.</source>
            <translation>Этап ротации отменён. Если его объявление будет доставлено позже, это устройство может никогда не прочитать ту ротацию.</translation>
        </message>
        <message>
            <source>Could not archive the chat. Try again.</source>
            <translation>Не удалось архивировать чат. Попробуйте снова.</translation>
        </message>
        <message>
            <source>Could not mark the chat as read. Try again.</source>
            <translation>Не удалось отметить чат как прочитанный. Попробуйте снова.</translation>
        </message>
        <message>
            <source>Could not rename the chat. Try again.</source>
            <translation>Не удалось переименовать чат. Попробуйте снова.</translation>
        </message>
    <message>
        <source>The original encryption key is unavailable. This pending message cannot be retried.</source>
        <translation>Исходный ключ шифрования недоступен. Повторная отправка этого сообщения невозможна.</translation>
    </message>
    <message>
        <source>Sending is unavailable. Your text is preserved.</source>
        <translation>Отправка недоступна. Ваш текст сохранён.</translation>
    </message>
    <message>
        <source>The pending message could not be saved. Your text is preserved.</source>
        <translation>Не удалось сохранить сообщение в очереди. Ваш текст сохранён.</translation>
    </message>
    <message>
        <source>The saved message was deleted. Your text is preserved.</source>
        <translation>Сохранённое сообщение было удалено. Ваш текст сохранён.</translation>
    </message>
    <message>
        <source>Pending messages could not be loaded. Sending is paused.</source>
        <translation>Не удалось загрузить очередь сообщений. Отправка приостановлена.</translation>
    </message>
    <message>
        <source>A pending message was restored. Check its status before retrying.</source>
        <translation>Восстановлено сообщение из очереди. Проверьте его статус перед повторной отправкой.</translation>
    </message>
    <message>
        <source>Message confirmation could not be saved. Check its status again.</source>
        <translation>Не удалось сохранить подтверждение сообщения. Проверьте его статус ещё раз.</translation>
    </message>
    <message>
        <source>The pending message could not be removed.</source>
        <translation>Не удалось удалить сообщение из очереди.</translation>
    </message>
    <message>
        <source>Pending messages could not be removed. The saved account was kept.</source>
        <translation>Не удалось удалить очередь сообщений. Сохранённый аккаунт оставлен.</translation>
    </message>
    <message>
        <source>There is nothing to forward.</source>
        <translation>Нечего пересылать.</translation>
    </message>
    <message>
        <source>The messages to forward could not be loaded. %1</source>
        <translation>Не удалось загрузить сообщения для пересылки. %1</translation>
    </message>
    <message>
        <source>The forwarded text could not be built.</source>
        <translation>Не удалось сформировать текст пересылки.</translation>
    </message>
    <message>
        <source>The forward could not be sent. %1</source>
        <translation>Не удалось отправить пересылку. %1</translation>
    </message>
    <message>
        <source>Forwarding was interrupted by a session change.</source>
        <translation>Пересылка прервана из-за смены сеанса.</translation>
    </message>
    <message>
        <source>Forwarding was not confirmed. Check does not send it again.</source>
        <translation>Пересылка не подтверждена. Проверка не отправляет её повторно.</translation>
    </message>
    <message>
        <source>The forwarded message could not be verified. Check does not send it again.</source>
        <translation>Пересланное сообщение не подтверждено. Проверка не отправляет его повторно.</translation>
    </message>
    <message>
        <source>Forwarding to encrypted chats is not supported.</source>
        <translation>Пересылка в зашифрованные чаты не поддерживается.</translation>
    </message>
    <message>
        <source>Forwarding to an unknown destination is not supported.</source>
        <translation>Пересылка в неизвестное назначение не поддерживается.</translation>
    </message>
    <message>
        <source>Only text messages and polls can be forwarded.</source>
        <translation>Пересылать можно только текстовые сообщения и опросы.</translation>
    </message>
    <message>
        <source>Could not create folder</source>
        <translation>Не удалось создать папку</translation>
    </message>
    <message>
        <source>Could not update folder</source>
        <translation>Не удалось обновить папку</translation>
    </message>
    <message>
        <source>Could not delete folder</source>
        <translation>Не удалось удалить папку</translation>
    </message>
    <message>
        <source>The folder change was interrupted by a session refresh. Try again.</source>
        <translation>Изменение папки прервано обновлением сессии. Повторите попытку.</translation>
    </message>
</context>
    <context>
        <name>workspace::NativeDesktop</name>
        <message>
            <source>Unread messages</source>
            <translation>Непрочитанные сообщения</translation>
        </message>
        <message>
            <source>Check for updates</source>
            <translation>Проверить обновления</translation>
        </message>
        <message>
            <source>Workspace</source>
            <translation>Workspace</translation>
        </message>
        <message>
            <source>%1 - In call: %2</source>
            <translation>%1 - идёт звонок: %2</translation>
        </message>
        <message>
            <source> (%1 participants)</source>
            <translation> (%1 участников)</translation>
        </message>
        <message>
            <source>Messenger</source>
            <translation>Мессенджер</translation>
        </message>
        <message>
            <source>Calendar</source>
            <translation>Календарь</translation>
        </message>
        <message>
            <source>Mail</source>
            <translation>Почта</translation>
        </message>
        <message>
            <source>Quit</source>
            <translation>Выход</translation>
        </message>
    </context>
    <context>
        <name>workspace::NativeInstance</name>
        <message>
            <source>The private application instance directory is unavailable.</source>
            <translation>Каталог приватного экземпляра приложения недоступен.</translation>
        </message>
        <message>
            <source>The application instance lock could not be opened.</source>
            <translation>Не удалось открыть блокировку экземпляра приложения.</translation>
        </message>
        <message>
            <source>The running application did not confirm activation. Try again shortly.</source>
            <translation>Запущенное приложение не подтвердило активацию. Повторите попытку через некоторое время.</translation>
        </message>
        <message>
            <source>The private application activation service could not start.</source>
            <translation>Не удалось запустить приватную службу активации приложения.</translation>
        </message>
    </context>
    <context>
        <name>workspace::NativeMarkdown</name>
        <message>
            <source>Spoiler</source>
            <translation>Спойлер</translation>
        </message>
        <message>
            <source>Could not display this message.</source>
            <translation>Не удалось отобразить сообщение.</translation>
        </message>
        <message>
            <source>Message is too large to display.</source>
            <translation>Сообщение слишком большое для отображения.</translation>
        </message>
        <message>
            <source>Encrypted fragment</source>
            <translation>Зашифрованный фрагмент</translation>
        </message>
        <message>
            <source>Image</source>
            <translation>Изображение</translation>
        </message>
    </context>
    <context>
        <name>workspace::NativePreferences</name>
        <message>
            <source>The local settings could not be cleared.</source>
            <translation>Не удалось очистить локальные настройки.</translation>
        </message>
        <message>
            <source>Saved appearance preferences could not be read.</source>
            <translation>Не удалось прочитать сохранённые параметры внешнего вида.</translation>
        </message>
        <message>
            <source>The appearance preference scope is invalid.</source>
            <translation>Не удалось определить аккаунт для настроек оформления.</translation>
        </message>
        <message>
            <source>Appearance preferences could not be saved.</source>
            <translation>Не удалось сохранить параметры внешнего вида.</translation>
        </message>
        <message>
            <source>The cache limit could not be saved.</source>
            <translation>Не удалось сохранить лимит кэша.</translation>
        </message>
        <message>
            <source>Reaction history could not be saved.</source>
            <translation>Не удалось сохранить историю реакций.</translation>
        </message>
        <message>
            <source>Window preferences could not be saved.</source>
            <translation>Не удалось сохранить параметры окна.</translation>
        </message>
    </context>
    <context>
        <name>workspace::Session</name>
    <message>
        <source>Login failed</source>
        <translation>Ошибка входа</translation>
    </message>
        <message>
            <source>Could not load organization settings. Check the server address and try again.</source>
            <translation>Не удалось загрузить настройки организации. Проверьте адрес сервера и попробуйте ещё раз.</translation>
        </message>
        <message>
            <source>Your session has expired. Sign in again.</source>
            <translation>Срок действия сеанса истёк. Войдите снова.</translation>
        </message>
        <message>
            <source>Saved sessions are unavailable.</source>
            <translation>Сохранённые сеансы недоступны.</translation>
        </message>
        <message>
            <source>The saved account identity is invalid.</source>
            <translation>Сохранённый идентификатор учётной записи недействителен.</translation>
        </message>
        <message>
            <source>The saved credential could not be removed. %1</source>
            <translation>Не удалось удалить сохранённые учётные данные. %1</translation>
        </message>
        <message>
            <source>Signed out, but the saved credential could not be removed. %1</source>
            <translation>Вы вышли из системы, но сохранённые учётные данные не удалось удалить. %1</translation>
        </message>
        <message>
            <source>Signed in, but this session could not be remembered. %1</source>
            <translation>Вы вошли в систему, но этот сеанс не удалось сохранить. %1</translation>
        </message>
        <message>
            <source>Enter your username, password and, if required, a six-digit code.</source>
            <translation>Введите имя пользователя, пароль и, при необходимости, шестизначный код.</translation>
        </message>
        <message>
            <source>Sign-in was rejected. Check your credentials and one-time code.</source>
            <translation>Вход отклонён. Проверьте учётные данные и одноразовый код.</translation>
        </message>
        <message>
            <source>Sign-in failed. %1</source>
            <translation>Не удалось войти. %1</translation>
        </message>
        <message>
            <source>The server returned an invalid token response.</source>
            <translation>Сервер вернул недопустимый ответ с токеном.</translation>
        </message>
        <message>
            <source>Could not load your projects. %1</source>
            <translation>Не удалось загрузить ваши проекты. %1</translation>
        </message>
        <message>
            <source>The server returned an invalid project list.</source>
            <translation>Сервер вернул недопустимый список проектов.</translation>
        </message>
        <message>
            <source>No accessible Workspace projects were returned.</source>
            <translation>Не получено ни одного доступного проекта Workspace.</translation>
        </message>
        <message>
            <source>Select an accessible project from the list.</source>
            <translation>Выберите доступный проект из списка.</translation>
        </message>
        <message>
            <source>The connection was interrupted while renewing your session. Retrying shortly.</source>
            <translation>Соединение прервалось при обновлении сеанса. Повторим попытку через некоторое время.</translation>
        </message>
        <message>
            <source>Your session could not be renewed. Sign in again.</source>
            <translation>Не удалось обновить сеанс. Войдите снова.</translation>
        </message>
        <message>
            <source>The server did not confirm the selected user and project.</source>
            <translation>Сервер не подтвердил выбранного пользователя и проект.</translation>
        </message>
    </context>
    <context>
        <name>workspace::SessionVault</name>
        <message>
            <source>The saved credential is no longer available.</source>
            <translation>Сохранённые учётные данные больше недоступны.</translation>
        </message>
        <message>
            <source>The operating system's secure credential store is unavailable.</source>
            <translation>Защищённое хранилище учётных данных операционной системы недоступно.</translation>
        </message>
        <message>
            <source>Access to the operating system's secure credential store was denied.</source>
            <translation>Доступ к защищённому хранилищу учётных данных операционной системы запрещён.</translation>
        </message>
        <message>
            <source>The desktop login keyring is locked. Unlock it to manage saved sign-ins.</source>
            <translation>Связка ключей для входа в систему заблокирована. Разблокируйте её, чтобы управлять сохранёнными учётными записями.</translation>
        </message>
        <message>
            <source>The secure credential operation failed.</source>
            <translation>Не удалось выполнить операцию с защищённым хранилищем.</translation>
        </message>
        <message>
            <source>Private saved-account metadata could not be opened.</source>
            <translation>Не удалось открыть сведения о сохранённых аккаунтах.</translation>
        </message>
        <message>
            <source>Saved-account metadata could not be read.</source>
            <translation>Не удалось прочитать сведения о сохранённых аккаунтах.</translation>
        </message>
        <message>
            <source>Saved-account metadata is not a regular private file.</source>
            <translation>Файл сведений о сохранённых аккаунтах не соответствует требованиям безопасности.</translation>
        </message>
        <message>
            <source>Saved-account metadata could not be updated.</source>
            <translation>Не удалось обновить сведения о сохранённых аккаунтах.</translation>
        </message>
        <message>
            <source>The saved account identity or credential is invalid.</source>
            <translation>Неверные данные сохранённого аккаунта.</translation>
        </message>
        <message>
            <source>The saved-account operation was superseded.</source>
            <translation>Операция с сохранённым аккаунтом заменена новой.</translation>
        </message>
        <message>
            <source>The saved account is no longer available.</source>
            <translation>Сохранённый аккаунт больше недоступен.</translation>
        </message>
        <message>
            <source>The saved account identity is invalid.</source>
            <translation>Сохранённый идентификатор учётной записи недействителен.</translation>
        </message>
    </context>
    <context>
        <name>NativeUpdateDialog</name>
        <message>
            <source>Workspace updates</source>
            <translation>Обновления Workspace</translation>
        </message>
        <message>
            <source>Current version: %1</source>
            <translation>Текущая версия: %1</translation>
        </message>
        <message>
            <source>Checking for updates…</source>
            <translation>Проверка обновлений…</translation>
        </message>
        <message>
            <source>You are using the latest version.</source>
            <translation>Вы используете последнюю версию.</translation>
        </message>
        <message>
            <source>Version %1 is available.</source>
            <translation>Доступна версия %1.</translation>
        </message>
        <message>
            <source>Downloading update… %1%</source>
            <translation>Загрузка обновления… %1%</translation>
        </message>
        <message>
            <source>The update is downloaded and verified.</source>
            <translation>Обновление загружено и проверено.</translation>
        </message>
        <message>
            <source>Installing update…</source>
            <translation>Установка обновления…</translation>
        </message>
        <message>
            <source>The update is installed. Restart to use it.</source>
            <translation>Обновление установлено. Перезапустите приложение.</translation>
        </message>
        <message>
            <source>Check GitHub for a new version.</source>
            <translation>Проверьте наличие новой версии на GitHub.</translation>
        </message>
        <message>
            <source>Close</source>
            <translation>Закрыть</translation>
        </message>
        <message>
            <source>Download update</source>
            <translation>Загрузить обновление</translation>
        </message>
        <message>
            <source>Install update</source>
            <translation>Установить обновление</translation>
        </message>
        <message>
            <source>Restart now</source>
            <translation>Перезапустить</translation>
        </message>
        <message>
            <source>Check again</source>
            <translation>Проверить снова</translation>
        </message>
    </context>
    <context>
        <name>workspace::NativeUpdater</name>
        <message>
            <source>The downloaded update failed verification.</source>
            <translation>Загруженное обновление не прошло проверку.</translation>
        </message>
        <message>
            <source>The installed update could not be verified.</source>
            <translation>Не удалось проверить установленное обновление.</translation>
        </message>
        <message>
            <source>The update archive layout is invalid.</source>
            <translation>Некорректная структура архива обновления.</translation>
        </message>
        <message>
            <source>The update configuration is invalid.</source>
            <translation>Некорректная конфигурация обновления.</translation>
        </message>
        <message>
            <source>The update could not be prepared.</source>
            <translation>Не удалось подготовить обновление.</translation>
        </message>
        <message>
            <source>The update download could not be saved.</source>
            <translation>Не удалось сохранить обновление.</translation>
        </message>
        <message>
            <source>The update download redirect was rejected.</source>
            <translation>Перенаправление загрузки обновления отклонено.</translation>
        </message>
        <message>
            <source>The update installation failed.</source>
            <translation>Не удалось установить обновление.</translation>
        </message>
        <message>
            <source>The update installer could not be started.</source>
            <translation>Не удалось запустить установку обновления.</translation>
        </message>
        <message>
            <source>The update installer produced too much output.</source>
            <translation>Установщик обновления превысил допустимый объём вывода.</translation>
        </message>
        <message>
            <source>The update operation timed out.</source>
            <translation>Превышено время ожидания обновления.</translation>
        </message>
        <message>
            <source>The update release metadata is invalid.</source>
            <translation>Некорректные сведения о выпуске обновления.</translation>
        </message>
        <message>
            <source>The update request failed.</source>
            <translation>Не удалось получить обновление.</translation>
        </message>
        <message>
            <source>The update response encoding was rejected.</source>
            <translation>Кодировка ответа сервера обновлений отклонена.</translation>
        </message>
        <message>
            <source>The update response is too large.</source>
            <translation>Ответ сервера обновлений слишком большой.</translation>
        </message>
        <message>
            <source>Updates are not available for this platform.</source>
            <translation>Обновления для этой платформы пока недоступны.</translation>
        </message>
    </context>
    <context>
        <name>NativeAboutDrawer</name>
        <message>
            <source>Version %1 available</source>
            <translation>Доступна версия %1</translation>
        </message>
        <message>
            <source>Downloading update… %1%</source>
            <translation>Загрузка обновления… %1%</translation>
        </message>
        <message>
            <source>Update ready to install</source>
            <translation>Обновление готово к установке</translation>
        </message>
        <message>
            <source>Application update</source>
            <translation>Обновление приложения</translation>
        </message>
        <message>
            <source>Back</source>
            <translation>Назад</translation>
        </message>
        <message>
            <source>App version</source>
            <translation>Версия приложения</translation>
        </message>
        <message>
            <source>Close</source>
            <translation>Закрыть</translation>
        </message>
        <message>
            <source>Current version: %1</source>
            <translation>Текущая версия: %1</translation>
        </message>
        <message>
            <source>Checking for updates…</source>
            <translation>Проверка обновлений…</translation>
        </message>
        <message>
            <source>The update is installed. Restart to use it.</source>
            <translation>Обновление установлено. Перезапустите приложение.</translation>
        </message>
        <message>
            <source>You&#x27;re up to date</source>
            <translation>Установлена последняя версия</translation>
        </message>
        <message>
            <source>Failed to check updates</source>
            <translation>Не удалось проверить обновления</translation>
        </message>
        <message>
            <source>Check whether a new version is available</source>
            <translation>Проверьте, доступна ли новая версия</translation>
        </message>
        <message>
            <source>Installation may take up to a minute. The application will restart automatically.</source>
            <translation>Установка займёт до минуты. Приложение запустится автоматически.</translation>
        </message>
        <message>
            <source>Install and restart</source>
            <translation>Установить и перезапустить</translation>
        </message>
        <message>
            <source>Restart now</source>
            <translation>Перезапустить</translation>
        </message>
        <message>
            <source>Check for updates</source>
            <translation>Проверить обновления</translation>
        </message>
    </context>
    <context>
        <name>NativeMessageImage</name>
        <message>
            <source>Retry</source>
            <translation>Повторить</translation>
        </message>
    </context>
    <context>
        <name>NativeDraftsPage</name>
        <message>
            <source>Waiting to save</source>
            <translation>Ожидает сохранения</translation>
        </message>
        <message>
            <source>Saving</source>
            <translation>Сохраняется</translation>
        </message>
        <message>
            <source>Could not delete</source>
            <translation>Не удалось удалить</translation>
        </message>
        <message>
            <source>Could not save</source>
            <translation>Не удалось сохранить</translation>
        </message>
        <message>
            <source>Conflict: remote version was kept</source>
            <translation>Конфликт: сохранена версия с сервера</translation>
        </message>
        <message>
            <source>Deleting</source>
            <translation>Удаляется</translation>
        </message>
        <message>
            <source>Drafts</source>
            <translation>Черновики</translation>
        </message>
        <message>
            <source>Refresh</source>
            <translation>Обновить</translation>
        </message>
        <message>
            <source>Close drafts</source>
            <translation>Закрыть черновики</translation>
        </message>
        <message>
            <source>Loading…</source>
            <translation>Загрузка…</translation>
        </message>
        <message>
            <source>No drafts yet</source>
            <translation>Черновиков пока нет</translation>
        </message>
        <message>
            <source>Edit draft</source>
            <translation>Редактировать черновик</translation>
        </message>
        <message>
            <source>Direct chat</source>
            <translation>Личный чат</translation>
        </message>
        <message>
            <source>Channel</source>
            <translation>Канал</translation>
        </message>
        <message>
            <source>Delete draft</source>
            <translation>Удалить черновик</translation>
        </message>
        <message>
            <source>Use server version</source>
            <translation>Принять версию сервера</translation>
        </message>
        <message>
            <source>Keep my version</source>
            <translation>Оставить мою версию</translation>
        </message>
        <message>
            <source>Retry deletion</source>
            <translation>Повторить удаление</translation>
        </message>
        <message>
            <source>Previous</source>
            <translation>Назад</translation>
        </message>
        <message>
            <source>Next</source>
            <translation>Далее</translation>
        </message>
    </context>
    <context>
        <name>NativeMentionsPage</name>
        <message>
            <source>Mentions</source>
            <translation>Упоминания</translation>
        </message>
        <message>
            <source>Close mentions</source>
            <translation>Закрыть упоминания</translation>
        </message>
        <message>
            <source>Filter mentions</source>
            <translation>Фильтр упоминаний</translation>
        </message>
        <message>
            <source>All</source>
            <translation>Все</translation>
        </message>
        <message>
            <source>Unread</source>
            <translation>Непрочитанные</translation>
        </message>
        <message>
            <source>Unread mention</source>
            <translation>Непрочитанное упоминание</translation>
        </message>
        <message>
            <source>No mentions yet</source>
            <translation>Упоминаний пока нет</translation>
        </message>
        <message>
            <source>No unread mentions</source>
            <translation>Непрочитанных упоминаний нет</translation>
        </message>
        <message>
            <source>Could not load activity messages.</source>
            <translation>Не удалось загрузить сообщения активности.</translation>
        </message>
        <message>
            <source>Could not refresh activity messages.</source>
            <translation>Не удалось обновить сообщения активности.</translation>
        </message>
        <message>
            <source>Could not load older messages.</source>
            <translation>Не удалось загрузить более ранние сообщения.</translation>
        </message>
        <message>
            <source>Retry</source>
            <translation>Повторить</translation>
        </message>
        <message>
            <source>Loading…</source>
            <translation>Загрузка…</translation>
        </message>
        <message>
            <source>Private</source>
            <translation>Личное</translation>
        </message>
    </context>
    <context>
        <name>NativeActivityUnsupportedPage</name>
        <message>
            <source>Reactions</source>
            <translation>Реакции</translation>
        </message>
        <message>
            <source>Reactions are not connected to Workspace messaging yet.</source>
            <translation>Реакции пока не подключены к Workspace-мессенджеру.</translation>
        </message>
        <message>
            <source>Close reactions</source>
            <translation>Закрыть реакции</translation>
        </message>
    </context>
    <context>
        <name>NativeStarredPage</name>
        <message>
            <source>Marked messages</source>
            <translation>Отмеченные сообщения</translation>
        </message>
        <message>
            <source>Close marked messages</source>
            <translation>Закрыть отмеченные сообщения</translation>
        </message>
        <message>
            <source>Could not load activity messages.</source>
            <translation>Не удалось загрузить сообщения активности.</translation>
        </message>
        <message>
            <source>Could not refresh activity messages.</source>
            <translation>Не удалось обновить сообщения активности.</translation>
        </message>
        <message>
            <source>Could not load older messages.</source>
            <translation>Не удалось загрузить более старые сообщения.</translation>
        </message>
        <message>
            <source>Retry</source>
            <translation>Повторить</translation>
        </message>
        <message>
            <source>No messages here yet...</source>
            <translation>Здесь пока нет сообщений...</translation>
        </message>
        <message>
            <source>Loading…</source>
            <translation>Загрузка…</translation>
        </message>
        <message>
            <source>Private</source>
            <translation>Личное</translation>
        </message>
        <message>
            <source>Open in chat</source>
            <translation>Открыть в чате</translation>
        </message>
        <message>
            <source>Forward</source>
            <translation>Переслать</translation>
        </message>
    </context>
    <context>
        <name>NativeInboxPage</name>
        <message>
            <source>Inbox</source>
            <translation>Входящие</translation>
        </message>
        <message>
            <source>Close inbox</source>
            <translation>Закрыть входящие</translation>
        </message>
        <message>
            <source>Direct messages</source>
            <translation>Личные сообщения</translation>
        </message>
        <message>
            <source>Channels</source>
            <translation>Каналы</translation>
        </message>
        <message>
            <source>No unread messages</source>
            <translation>Нет непрочитанных сообщений</translation>
        </message>
        <message>
            <source>Failed to load inbox</source>
            <translation>Не удалось загрузить входящие</translation>
        </message>
        <message>
            <source>Loading…</source>
            <translation>Загрузка…</translation>
        </message>
    </context>
    <context>
        <name>NativeFeedPage</name>
        <message>
            <source>All messages</source>
            <translation>Все сообщения</translation>
        </message>
        <message>
            <source>Close feed</source>
            <translation>Закрыть ленту</translation>
        </message>
        <message>
            <source>Loading…</source>
            <translation>Загрузка…</translation>
        </message>
        <message>
            <source>Failed to load messages</source>
            <translation>Не удалось загрузить сообщения</translation>
        </message>
        <message>
            <source>Retry</source>
            <translation>Повторить</translation>
        </message>
        <message>
            <source>No messages yet</source>
            <translation>Пока нет сообщений</translation>
        </message>
        <message>
            <source>Open in chat</source>
            <translation>Открыть в чате</translation>
        </message>
        <message>
            <source>Forward</source>
            <translation>Переслать</translation>
        </message>
    </context>
    <context>
        <name>workspace::NotificationService</name>
        <message>
            <source>New message</source>
            <translation>Новое сообщение</translation>
        </message>
        <message>
            <source>You have a new message</source>
            <translation>У вас новое сообщение</translation>
        </message>
        <message numerus="yes">
            <source>%n message</source>
            <translation>
                <numerusform>%n сообщение</numerusform>
                <numerusform>%n сообщения</numerusform>
                <numerusform>%n сообщений</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>%n messages</source>
            <translation>
                <numerusform>%n сообщение</numerusform>
                <numerusform>%n сообщения</numerusform>
                <numerusform>%n сообщений</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>%n message from %1</source>
            <translation>
                <numerusform>%n сообщение от %1</numerusform>
                <numerusform>%n сообщения от %1</numerusform>
                <numerusform>%n сообщений от %1</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>%n messages from %1</source>
            <translation>
                <numerusform>%n сообщение от %1</numerusform>
                <numerusform>%n сообщения от %1</numerusform>
                <numerusform>%n сообщений от %1</numerusform>
            </translation>
        </message>
    </context>
    <context>
        <name>NativeCallsPage</name>
        <message>
            <source>Recent Jitsi calls</source>
            <translation>Последние звонки Jitsi</translation>
        </message>
        <message>
            <source>No recent Jitsi calls yet</source>
            <translation>Пока нет недавних звонков Jitsi</translation>
        </message>
    </context>
    <context>
        <name>NativeServicesPage</name>
        <message>
            <source>Services</source>
            <translation>Сервисы</translation>
        </message>
        <message>
            <source>Services will be available here. Integrations are being prepared.</source>
            <translation>Здесь появятся сервисы. Интеграции уже готовятся.</translation>
        </message>
        <message>
            <source>Knowledge base</source>
            <translation>База знаний</translation>
        </message>
        <message>
            <source>Internal docs and team playbooks.</source>
            <translation>Внутренние документы и командные инструкции.</translation>
        </message>
        <message>
            <source>People directory</source>
            <translation>Справочник сотрудников</translation>
        </message>
        <message>
            <source>Team structure, contacts, and roles.</source>
            <translation>Структура команды, контакты и роли.</translation>
        </message>
        <message>
            <source>Approvals</source>
            <translation>Согласования</translation>
        </message>
        <message>
            <source>Requests and approval workflows.</source>
            <translation>Заявки и процессы согласования.</translation>
        </message>
        <message>
            <source>Document vault</source>
            <translation>Документы</translation>
        </message>
        <message>
            <source>Contracts, templates, and shared files.</source>
            <translation>Договоры, шаблоны и общие файлы.</translation>
        </message>
        <message>
            <source>Automation</source>
            <translation>Автоматизация</translation>
        </message>
        <message>
            <source>Routine process automation and bots.</source>
            <translation>Автоматизация рутинных процессов и боты.</translation>
        </message>
        <message>
            <source>Integrations</source>
            <translation>Интеграции</translation>
        </message>
        <message>
            <source>Connections with external SaaS systems.</source>
            <translation>Подключения к внешним SaaS-системам.</translation>
        </message>
        <message>
            <source>Coming soon</source>
            <translation>Скоро</translation>
        </message>
        <message>
            <source>Chats &amp; channels</source>
            <translation>Чаты и каналы</translation>
        </message>
    </context>
    <context>
        <name>NativeMediaViewer</name>
        <message>
            <source>Media viewer</source>
            <translation>Просмотр медиа</translation>
        </message>
        <message>
            <source>Loading media…</source>
            <translation>Медиа загружается…</translation>
        </message>
        <message>
            <source>Video is loading…</source>
            <translation>Видео загружается…</translation>
        </message>
        <message>
            <source>Failed to load video</source>
            <translation>Не удалось загрузить видео</translation>
        </message>
        <message>
            <source>This video cannot be played</source>
            <translation>Не удалось воспроизвести видео</translation>
        </message>
        <message>
            <source>Play</source>
            <translation>Воспроизвести</translation>
        </message>
        <message>
            <source>Pause</source>
            <translation>Пауза</translation>
        </message>
        <message>
            <source>Mute</source>
            <translation>Выключить звук</translation>
        </message>
        <message>
            <source>Unmute</source>
            <translation>Включить звук</translation>
        </message>
        <message>
            <source>Volume</source>
            <translation>Громкость</translation>
        </message>
        <message>
            <source>Seek</source>
            <translation>Перемотка</translation>
        </message>
        <message>
            <source>Retry</source>
            <translation>Повторить</translation>
        </message>
        <message>
            <source>This media cannot be displayed. You can download the file instead.</source>
            <translation>Не удалось показать медиа. Файл можно скачать.</translation>
        </message>
        <message>
            <source>Previous</source>
            <translation>Предыдущее</translation>
        </message>
        <message>
            <source>Next</source>
            <translation>Следующее</translation>
        </message>
        <message>
            <source>Download</source>
            <translation>Скачать</translation>
        </message>
        <message>
            <source>Copy image</source>
            <translation>Копировать изображение</translation>
        </message>
        <message>
            <source>Close</source>
            <translation>Закрыть</translation>
        </message>
        <message>
            <source>Media %1</source>
            <translation>Медиа %1</translation>
        </message>
    </context>
<context>
    <name>NativeExternalAccountBlock</name>
    <message><source>Integration settings</source><translation>Настройки интеграций</translation></message>
    <message><source>Organization-wide external service settings</source><translation>Общие настройки внешних сервисов организации</translation></message>
    <message><source>External accounts</source><translation>Внешние аккаунты</translation></message>
    <message><source>Could not load external accounts. Check access and try again.</source><translation>Не удалось загрузить внешние аккаунты. Проверь доступ и повтори попытку.</translation></message>
    <message><source>Checking…</source><translation>Проверяем…</translation></message>
    <message><source>No external accounts connected yet.</source><translation>Внешние аккаунты ещё не подключены.</translation></message>
    <message><source>Connected</source><translation>Подключены</translation></message>
    <message><source>External account actions</source><translation>Действия внешнего аккаунта</translation></message>
    <message><source>Edit</source><translation>Изменить</translation></message>
    <message><source>Reconnect</source><translation>Переподключить</translation></message>
    <message><source>Delete</source><translation>Удалить</translation></message>
    <message><source>Linked services let you sign in to the same profile from different messengers without setting it up again</source><translation>Привязанные сервисы позволяют входить в один и тот же профиль с разных мессенджеров без повторной настройки</translation></message>
    <message><source>Connect service</source><translation>Подключить сервис</translation></message>
    <message><source>Checking connection…</source><translation>Проверяем подключение…</translation></message>
    <message><source>Loading history…</source><translation>Загружаем историю…</translation></message>
    <message><source>Connection is active</source><translation>Подключение активно</translation></message>
    <message><source>Connection needs attention</source><translation>Подключение требует внимания</translation></message>
    <message><source>Zulip credentials need to be updated</source><translation>Нужно обновить данные доступа к Zulip</translation></message>
    <message><source>Synchronization is stopped</source><translation>Синхронизация остановлена</translation></message>
    <message><source>The integration was suspended by an administrator</source><translation>Интеграция приостановлена администратором</translation></message>
    <message><source>Edit external account</source><translation>Редактировать внешний аккаунт</translation></message>
    <message><source>Connect external account</source><translation>Подключить внешний аккаунт</translation></message>
    <message><source>Provider</source><translation>Провайдер</translation></message>
    <message><source>Zulip is already connected.</source><translation>Zulip уже подключён.</translation></message>
    <message><source>Server URL</source><translation>URL сервера</translation></message>
    <message><source>Email</source><translation>Email</translation></message>
    <message><source>API key</source><translation>API-ключ</translation></message>
    <message><source>Could not update the external account. Check the details and try again.</source><translation>Не удалось обновить внешний аккаунт. Проверьте данные и попробуйте ещё раз.</translation></message>
    <message><source>Cancel</source><translation>Отмена</translation></message>
    <message><source>Save</source><translation>Сохранить</translation></message>
    <message><source>Connect</source><translation>Подключить</translation></message>
    <message><source>Delete external account?</source><translation>Удалить внешний аккаунт?</translation></message>
    <message><source>Could not delete the external account.</source><translation>Не удалось удалить внешний аккаунт.</translation></message>
</context>
    <context>
        <name>NativeRightDrawer</name>
        <message>
            <source>Info panel</source>
            <translation>Панель информации</translation>
        </message>
        <message>
            <source>Back</source>
            <translation>Назад</translation>
        </message>
        <message>
            <source>Close</source>
            <translation>Закрыть</translation>
        </message>
    </context>
    <context>
        <name>NativeChannelTopics</name>
        <message><source>General chat</source><translation>Общий чат</translation></message>
        <message><source>Muted</source><translation>Без звука</translation></message>
        <message><source>Followed</source><translation>Отслеживается</translation></message>
        <message><source>Unmuted</source><translation>Со звуком</translation></message>
        <message><source>Default</source><translation>По умолчанию</translation></message>
        <message><source>Topics</source><translation>Темы</translation></message>
        <message><source>Collapse topics</source><translation>Свернуть темы</translation></message>
        <message><source>Expand topics</source><translation>Развернуть темы</translation></message>
        <message><source>No topics</source><translation>Нет тем</translation></message>
        <message><source>Topic notifications: %1</source><translation>Уведомления топика: %1</translation></message>
        <message><source>Could not update the topic. Please try again.</source><translation>Не удалось обновить топик. Попробуйте ещё раз.</translation></message>
        <message><source>Mark as read</source><translation>Отметить как прочитанное</translation></message>
        <message><source>Rename topic</source><translation>Переименовать тему</translation></message>
        <message><source>Mark topic as done</source><translation>Отметить топик завершённым</translation></message>
        <message><source>Save</source><translation>Сохранить</translation></message>
        <message><source>Cancel</source><translation>Отмена</translation></message>
    </context>
    <context>
        <name>NativeTopicSummary</name>
        <message><source>Topic context</source><translation>Контекст топика</translation></message>
        <message><source>Open AI summary settings</source><translation>Открыть настройки AI-выжимки</translation></message>
        <message><source>Collapse topic context</source><translation>Свернуть контекст топика</translation></message>
        <message><source>Expand topic context</source><translation>Развернуть контекст топика</translation></message>
        <message><source>The summary will appear after the messages are processed.</source><translation>Выжимка появится после обработки сообщений.</translation></message>
        <message><source>There is no summary for this topic.</source><translation>Для этой темы пока нет выжимки.</translation></message>
        <message><source>There are new messages that are not yet included in the summary.</source><translation>Есть новые сообщения, которые пока не вошли в выжимку.</translation></message>
        <message><source>Automatic summary updates are disabled for this topic.</source><translation>Автоматическое обновление выжимки для этой темы отключено.</translation></message>
        <message><source>AI summary settings</source><translation>Настройки AI-выжимки</translation></message>
        <message><source>Automatic summary updates</source><translation>Автоматическое обновление выжимки</translation></message>
        <message><source>Could not update the topic. Please try again.</source><translation>Не удалось обновить топик. Попробуйте ещё раз.</translation></message>
        <message><source>Close</source><translation>Закрыть</translation></message>
        <message><source>Save</source><translation>Сохранить</translation></message>
    </context>
    <context>
        <name>NativeChannelInfo</name>
        <message>
            <source>Call</source>
            <translation>Позвонить</translation>
        </message>
        <message>
            <source>Owner</source>
            <translation>Владелец</translation>
        </message>
        <message>
            <source>Administrator</source>
            <translation>Администратор</translation>
        </message>
        <message>
            <source>Moderator</source>
            <translation>Модератор</translation>
        </message>
        <message>
            <source>Guest</source>
            <translation>Гость</translation>
        </message>
        <message>
            <source>Member</source>
            <translation>Участник</translation>
        </message>
        <message>
            <source>online</source>
            <translation>В сети</translation>
        </message>
        <message>
            <source>away</source>
            <translation>Отошёл</translation>
        </message>
        <message>
            <source>offline</source>
            <translation>Не в сети</translation>
        </message>
        <message>
            <source>do not disturb</source>
            <translation>Не беспокоить</translation>
        </message>
        <message numerus="yes">
            <source>%n member</source>
            <translation>
                <numerusform>%n участник</numerusform>
                <numerusform>%n участника</numerusform>
                <numerusform>%n участников</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>%n members</source>
            <translation>
                <numerusform>%n участник</numerusform>
                <numerusform>%n участника</numerusform>
                <numerusform>%n участников</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>%n online</source>
            <translation>
                <numerusform>%n в сети</numerusform>
                <numerusform>%n в сети</numerusform>
                <numerusform>%n в сети</numerusform>
            </translation>
        </message>
        <message>
            <source>Notifications</source>
            <translation>Уведомления</translation>
        </message>
        <message>
            <source>For entire chat</source>
            <translation>Для всего чата</translation>
        </message>
        <message>
            <source>Mentions only</source>
            <translation>Только при упоминаниях</translation>
        </message>
        <message>
            <source>Muted</source>
            <translation>Без уведомлений</translation>
        </message>
        <message>
            <source>All messages</source>
            <translation>Все сообщения</translation>
        </message>
        <message>
            <source>This action is not available in this conversation yet.</source>
            <translation>Это действие пока недоступно в этом чате.</translation>
        </message>
        <message>
            <source>Description</source>
            <translation>Описание</translation>
        </message>
        <message>
            <source>Members</source>
            <translation>Участники</translation>
        </message>
        <message>
            <source>Add members</source>
            <translation>Добавить участников</translation>
        </message>
        <message>
            <source>No members found</source>
            <translation>Участники не найдены</translation>
        </message>
        <message>
            <source>Remove from channel: %1</source>
            <translation>Удалить из канала: %1</translation>
        </message>
        <message>
            <source>Search users…</source>
            <translation>Поиск пользователей…</translation>
        </message>
        <message>
            <source>No results found</source>
            <translation>Ничего не найдено</translation>
        </message>
        <message>
            <source>Close</source>
            <translation>Закрыть</translation>
        </message>
        <message>
            <source>Cancel</source>
            <translation>Отмена</translation>
        </message>
        <message>
            <source>Add</source>
            <translation>Добавить</translation>
        </message>
    </context>
<context>
    <name>NativeUserSearch</name>
    <message><source>Retry</source><translation>Повторить</translation></message>
    <message><source>Search</source><translation>Поиск</translation></message>
    <message><source>Start chat</source><translation>Начать чат</translation></message>
    <message><source>No results found</source><translation>Ничего не найдено</translation></message>
</context>
    <context>
        <name>NativeUserProfile</name>
        <message>
            <source>About me</source>
            <translation>О себе</translation>
        </message>
        <message>
            <source>Account status</source>
            <translation>Статус аккаунта</translation>
        </message>
        <message>
            <source>Account type</source>
            <translation>Тип аккаунта</translation>
        </message>
        <message>
            <source>Birthday</source>
            <translation>День рождения</translation>
        </message>
        <message>
            <source>Call</source>
            <translation>Позвонить</translation>
        </message>
        <message>
            <source>Cancel</source>
            <translation>Отмена</translation>
        </message>
        <message>
            <source>Change avatar</source>
            <translation>Изменить аватар</translation>
        </message>
        <message>
            <source>Check the lengths of the editable fields.</source>
            <translation>Проверьте длину редактируемых полей.</translation>
        </message>
        <message>
            <source>Choose from gallery</source>
            <translation>Выбрать из галереи</translation>
        </message>
        <message>
            <source>Copied</source>
            <translation>Скопировано</translation>
        </message>
        <message>
            <source>Delete current photo</source>
            <translation>Удалить текущее фото</translation>
        </message>
        <message>
            <source>Display name</source>
            <translation>Отображаемое имя</translation>
        </message>
        <message>
            <source>Edit</source>
            <translation>Редактировать</translation>
        </message>
        <message>
            <source>Edit avatar</source>
            <translation>Изменить аватар</translation>
        </message>
        <message>
            <source>Email</source>
            <translation>Email</translation>
        </message>
        <message>
            <source>Favorites</source>
            <translation>Избранное</translation>
        </message>
        <message>
            <source>First name</source>
            <translation>Имя</translation>
        </message>
        <message>
            <source>Joined</source>
            <translation>Дата регистрации</translation>
        </message>
        <message>
            <source>Last name</source>
            <translation>Фамилия</translation>
        </message>
        <message>
            <source>Loading profile…</source>
            <translation>Загрузка профиля…</translation>
        </message>
        <message>
            <source>Local time</source>
            <translation>Местное время</translation>
        </message>
        <message>
            <source>Manager</source>
            <translation>Руководитель</translation>
        </message>
        <message>
            <source>Message</source>
            <translation>Написать</translation>
        </message>
        <message>
            <source>Phone</source>
            <translation>Телефон</translation>
        </message>
        <message>
            <source>Profile saved. Workspace could not refresh it yet.</source>
            <translation>Профиль сохранён. Пока не удалось обновить его в Workspace.</translation>
        </message>
        <message>
            <source>Retry</source>
            <translation>Повторить</translation>
        </message>
        <message>
            <source>Role</source>
            <translation>Роль</translation>
        </message>
        <message>
            <source>Save changes</source>
            <translation>Сохранить изменения</translation>
        </message>
        <message>
            <source>Share</source>
            <translation>Поделиться</translation>
        </message>
        <message>
            <source>Team > Job title</source>
            <translation>Команда > Должность</translation>
        </message>
        <message>
            <source>Timezone</source>
            <translation>Часовой пояс</translation>
        </message>
        <message>
            <source>User ID</source>
            <translation>ID пользователя</translation>
        </message>
        <message>
            <source>Username</source>
            <translation>Имя пользователя</translation>
        </message>
        <message>
            <source>away</source>
            <translation>Отошёл</translation>
        </message>
        <message>
            <source>do not disturb</source>
            <translation>Не беспокоить</translation>
        </message>
        <message>
            <source>offline</source>
            <translation>Не в сети</translation>
        </message>
        <message>
            <source>online</source>
            <translation>В сети</translation>
        </message>
    </context>
    <context>
        <name>NativeJitsiCard</name>
        <message>
            <source>Call</source>
            <translation>Звонок</translation>
        </message>
        <message>
            <source>Call name</source>
            <translation>Название звонка</translation>
        </message>
        <message>
            <source>Join call</source>
            <translation>Подключиться</translation>
        </message>
    </context>
    <context>
        <name>JitsiCallWindowController</name>
        <message>
            <source>Call</source>
            <translation>Позвонить</translation>
        </message>
        <message numerus="yes">
            <source>Call (%n participant)</source>
            <translation>
                <numerusform>Звонок (%n участник)</numerusform>
                <numerusform>Звонок (%n участника)</numerusform>
                <numerusform>Звонок (%n участников)</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>Call (%n participants)</source>
            <translation>
                <numerusform>Звонок (%n участник)</numerusform>
                <numerusform>Звонок (%n участника)</numerusform>
                <numerusform>Звонок (%n участников)</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>%n participant</source>
            <translation>
                <numerusform>%n участник</numerusform>
                <numerusform>%n участника</numerusform>
                <numerusform>%n участников</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>%n participants</source>
            <translation>
                <numerusform>%n участник</numerusform>
                <numerusform>%n участника</numerusform>
                <numerusform>%n участников</numerusform>
            </translation>
        </message>
        <message>
            <source>Participant</source>
            <translation>Участник</translation>
        </message>
        <message>
            <source>Invalid call link</source>
            <translation>Неверная ссылка на звонок</translation>
        </message>
        <message>
            <source>Loading call client…</source>
            <translation>Загрузка клиента звонков…</translation>
        </message>
        <message>
            <source>Could not load call client</source>
            <translation>Не удалось загрузить клиент звонков</translation>
        </message>
        <message>
            <source>Try again</source>
            <translation>Попробовать снова</translation>
        </message>
        <message>
            <source>Browser fullscreen mode</source>
            <translation>Полноэкранный режим браузера</translation>
        </message>
        <message>
            <source>Exit fullscreen</source>
            <translation>Выйти из полноэкранного</translation>
        </message>
        <message>
            <source>Expand</source>
            <translation>Развернуть</translation>
        </message>
        <message>
            <source>Minimize</source>
            <translation>Свернуть</translation>
        </message>
        <message>
            <source>Minimize to window</source>
            <translation>Свернуть в окно</translation>
        </message>
        <message>
            <source>Close call</source>
            <translation>Закрыть звонок</translation>
        </message>
    </context>
    <context>
        <name>workspace::ScreenShareSourcePicker</name>
        <message>
            <source>No display is available to share.</source>
            <translation>Нет доступного для демонстрации экрана.</translation>
        </message>
        <message>
            <source>Screen %1</source>
            <translation>Экран %1</translation>
        </message>
        <message>
            <source>Screen capture</source>
            <translation>Захват экрана</translation>
        </message>
        <message>
            <source>The screen share result did not include a selectable stream.</source>
            <translation>Результат выбора источника не содержит доступного для захвата потока.</translation>
        </message>
        <message>
            <source>The screen share source selection timed out.</source>
            <translation>Выбор источника демонстрации экрана завершился по истечении времени ожидания.</translation>
        </message>
        <message>
            <source>The screen share source selection was cancelled.</source>
            <translation>Выбор источника демонстрации экрана отменён.</translation>
        </message>
        <message>
            <source>The system screen picker could not be opened.</source>
            <translation>Не удалось открыть системный выбор экрана.</translation>
        </message>
    </context>
    <context>
        <name>workspace::JitsiCallWindowController</name>
        <message>
            <source>Call</source>
            <translation>Позвонить</translation>
        </message>
        <message numerus="yes">
            <source>Call (%n participant)</source>
            <translation>
                <numerusform>Звонок (%n участник)</numerusform>
                <numerusform>Звонок (%n участника)</numerusform>
                <numerusform>Звонок (%n участников)</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>Call (%n participants)</source>
            <translation>
                <numerusform>Звонок (%n участник)</numerusform>
                <numerusform>Звонок (%n участника)</numerusform>
                <numerusform>Звонок (%n участников)</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>%n participant</source>
            <translation>
                <numerusform>%n участник</numerusform>
                <numerusform>%n участника</numerusform>
                <numerusform>%n участников</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>%n participants</source>
            <translation>
                <numerusform>%n участник</numerusform>
                <numerusform>%n участника</numerusform>
                <numerusform>%n участников</numerusform>
            </translation>
        </message>
        <message>
            <source>Participant</source>
            <translation>Участник</translation>
        </message>
        <message>
            <source>Invalid call link</source>
            <translation>Неверная ссылка на звонок</translation>
        </message>
        <message>
            <source>Loading call client…</source>
            <translation>Загрузка клиента звонков…</translation>
        </message>
        <message>
            <source>Could not load call client</source>
            <translation>Не удалось загрузить клиент звонков</translation>
        </message>
        <message>
            <source>Try again</source>
            <translation>Попробовать снова</translation>
        </message>
        <message>
            <source>Browser fullscreen mode</source>
            <translation>Полноэкранный режим браузера</translation>
        </message>
        <message>
            <source>Exit fullscreen</source>
            <translation>Выйти из полноэкранного</translation>
        </message>
        <message>
            <source>Expand</source>
            <translation>Развернуть</translation>
        </message>
        <message>
            <source>Minimize</source>
            <translation>Свернуть</translation>
        </message>
        <message>
            <source>Minimize to window</source>
            <translation>Свернуть в окно</translation>
        </message>
        <message>
            <source>Close call</source>
            <translation>Закрыть звонок</translation>
        </message>
    </context>
    <context>
        <name>NativeIncomingCallWindow</name>
        <message>
            <source>Participant</source>
            <translation>Участник</translation>
        </message>
        <message>
            <source>Choose how to answer the call</source>
            <translation>Выберите удобный способ для звонка</translation>
        </message>
        <message>
            <source>With video</source>
            <translation>Вкл. видео</translation>
        </message>
        <message>
            <source>Cancel</source>
            <translation>Отменить</translation>
        </message>
        <message>
            <source>Accept</source>
            <translation>Принять</translation>
        </message>
        <message>
            <source>Decline</source>
            <translation>Отклонить</translation>
        </message>
        <message>
            <source>Incoming call</source>
            <translation>Входящий звонок</translation>
        </message>
        <message>
            <source>%1 is calling</source>
            <translation>%1 звонит</translation>
        </message>
    </context>
<context>
    <name>NativePollManagementDialog</name>
    <message><source>Close poll?</source><translation>Завершить опрос?</translation></message>
    <message><source>Edit poll</source><translation>Изменить опрос</translation></message>
    <message><source>Participants will no longer be able to vote or change their answers.</source><translation>Участники больше не смогут голосовать или менять свои ответы.</translation></message>
    <message><source>Poll closing date</source><translation>Дата завершения опроса</translation></message>
    <message><source>Remove deadline</source><translation>Убрать дату</translation></message>
    <message><source>Set a deadline</source><translation>Установить дату</translation></message>
    <message><source>Time</source><translation>Время</translation></message>
    <message><source>Choose a future date</source><translation>Выберите дату в будущем</translation></message>
    <message><source>Allow adding options</source><translation>Разрешить добавление вариантов</translation></message>
    <message><source>Participants can add answer options</source><translation>Участники могут добавлять варианты ответов</translation></message>
    <message><source>Could not save the change. Please try again.</source><translation>Не удалось сохранить изменение. Попробуйте ещё раз.</translation></message>
    <message><source>Cancel</source><translation>Отмена</translation></message>
    <message><source>Close poll</source><translation>Завершить опрос</translation></message>
    <message><source>Save</source><translation>Сохранить</translation></message>
</context>
    <context>
        <name>NativePollDialog</name>
        <message>
            <source>Could not confirm the poll was sent. Retry to safely send the same poll.</source>
            <translation>Не удалось подтвердить отправку опроса. Повторите отправку того же опроса.</translation>
        </message>
        <message>
            <source>The poll was rejected. Check its questions and settings, then try again.</source>
            <translation>Опрос отклонён. Проверьте вопросы и настройки, затем попробуйте снова.</translation>
        </message>
        <message>
            <source>Poll creation is not enabled on this server.</source>
            <translation>Создание опросов на этом сервере не включено.</translation>
        </message>
        <message>
            <source>The default server limit is 20 questions and 20 answers per question.</source>
            <translation>Стандартный лимит сервера — 20 вопросов и 20 ответов на вопрос.</translation>
        </message>
        <message>
            <source>Polls allow at most 20 questions and 20 answers per question. Remove some before sending.</source>
            <translation>Опрос допускает не более 20 вопросов и не более 20 ответов на вопрос. Удалите лишние перед отправкой.</translation>
        </message>
        <message>
            <source>Add question</source>
            <translation>Добавить вопрос</translation>
        </message>
        <message>
            <source>Poll settings</source>
            <translation>Настройки опроса</translation>
        </message>
        <message>
            <source>Multiple answers</source>
            <translation>Несколько ответов</translation>
        </message>
        <message>
            <source>Participants can select more than one answer for each question</source>
            <translation>Участники могут выбрать несколько ответов в каждом вопросе</translation>
        </message>
        <message>
            <source>Anonymous poll</source>
            <translation>Анонимный опрос</translation>
        </message>
        <message>
            <source>Hide voters</source>
            <translation>Скрыть участников голосования</translation>
        </message>
        <message>
            <source>Allow revoting</source>
            <translation>Разрешить переголосование</translation>
        </message>
        <message>
            <source>Votes can be changed or withdrawn</source>
            <translation>Можно изменить или отозвать голос</translation>
        </message>
        <message>
            <source>Allow adding options</source>
            <translation>Разрешить добавление вариантов</translation>
        </message>
        <message>
            <source>Participants can add answer options</source>
            <translation>Участники могут добавлять варианты ответа</translation>
        </message>
        <message>
            <source>Results</source>
            <translation>Результаты</translation>
        </message>
        <message>
            <source>After voting</source>
            <translation>После голосования</translation>
        </message>
        <message>
            <source>Always</source>
            <translation>Всегда</translation>
        </message>
        <message>
            <source>Poll closing date</source>
            <translation>Закрытие опроса</translation>
        </message>
        <message>
            <source>Choose a future date</source>
            <translation>Выберите будущую дату</translation>
        </message>
        <message>
            <source>Remove deadline</source>
            <translation>Убрать срок</translation>
        </message>
        <message>
            <source>Set a deadline</source>
            <translation>Установить срок</translation>
        </message>
        <message>
            <source>Cancel</source>
            <translation>Отмена</translation>
        </message>
        <message>
            <source>Create</source>
            <translation>Создать</translation>
        </message>
        <message>
            <source>Question</source>
            <translation>Вопрос</translation>
        </message>
        <message>
            <source>Enter a question</source>
            <translation>Введите вопрос</translation>
        </message>
        <message>
            <source>Answer options</source>
            <translation>Варианты ответа</translation>
        </message>
        <message>
            <source>Mark correct answers if needed</source>
            <translation>Отметьте правильные ответы, если нужно</translation>
        </message>
        <message>
            <source>Add an answer…</source>
            <translation>Добавить ответ…</translation>
        </message>
        <message>
            <source>Mark as correct</source>
            <translation>Отметить правильный ответ</translation>
        </message>
        <message>
            <source>Create poll</source>
            <translation>Создать опрос</translation>
        </message>
    </context>
    <context>
        <name>NativeQuoteMediaThumbnail</name>
        <message>
            <source>Image</source>
            <translation>Изображение</translation>
        </message>
        <message>
            <source>Video</source>
            <translation>Видео</translation>
        </message>
    </context>
<context>
    <name>NativePollVoters</name>
    <message><source>Voters</source><translation>Проголосовавшие</translation></message>
    <message><source>Close</source><translation>Закрыть</translation></message>
    <message><source>Answer</source><translation>Ответ</translation></message>
    <message><source>Loading…</source><translation>Загрузка…</translation></message>
    <message><source>Could not load voters. Please try again.</source><translation>Не удалось загрузить участников. Попробуйте ещё раз.</translation></message>
    <message><source>No votes yet</source><translation>Пока никто не проголосовал</translation></message>
    <message><source>Retry</source><translation>Повторить</translation></message>
    <message><source>Load more</source><translation>Загрузить ещё</translation></message>
</context>
    <context>
        <name>NativePollCard</name>
    <message><source>View votes</source><translation>Посмотреть голоса</translation></message>
    <message><source>Who chose “%1”</source><translation>Кто выбрал «%1»</translation></message>
        <message>
            <source>Poll closed</source>
            <translation>Опрос завершён</translation>
        </message>
        <message>
            <source>Anonymous poll</source>
            <translation>Анонимный опрос</translation>
        </message>
        <message>
            <source>Public poll</source>
            <translation>Публичный опрос</translation>
        </message>
        <message>
            <source>Choose multiple options</source>
            <translation>Выберите несколько вариантов</translation>
        </message>
        <message>
            <source>Choose one option</source>
            <translation>Выберите один вариант</translation>
        </message>
        <message>
            <source>Questions: %1</source>
            <translation>Вопросы: %1</translation>
        </message>
        <message>
            <source>Closed %1</source>
            <translation>Завершён %1</translation>
        </message>
        <message>
            <source>Until %1</source>
            <translation>До %1</translation>
        </message>
        <message numerus="yes">
            <source>%n participant</source>
            <translation>
                <numerusform>%n участник</numerusform>
                <numerusform>%n участника</numerusform>
                <numerusform>%n участников</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>%n participants</source>
            <translation>
                <numerusform>%n участник</numerusform>
                <numerusform>%n участника</numerusform>
                <numerusform>%n участников</numerusform>
            </translation>
        </message>
        <message>
            <source>You voted</source>
            <translation>Вы проголосовали</translation>
        </message>
        <message>
            <source>Results after voting</source>
            <translation>Результаты после голосования</translation>
        </message>
        <message>
            <source>Could not save the change. Please try again.</source>
            <translation>Не удалось сохранить изменение. Попробуйте снова.</translation>
        </message>
        <message>
            <source>Could not add the option. Try again.</source>
            <translation>Не удалось добавить вариант. Попробуйте снова.</translation>
        </message>
        <message>
            <source>Correct answer: %1</source>
            <translation>Правильный ответ: %1</translation>
        </message>
        <message>
            <source>Participants: %1</source>
            <translation>Участников: %1</translation>
        </message>
        <message>
            <source>Add an answer option…</source>
            <translation>Добавить вариант ответа…</translation>
        </message>
        <message>
            <source>Press Enter to add an answer. Shift+Enter inserts a new line.</source>
            <translation>Нажмите Enter, чтобы добавить вариант ответа. Shift+Enter вставляет новую строку.</translation>
        </message>
        <message>
            <source>Vote counts</source>
            <translation>Результаты голосования</translation>
        </message>
        <message>
            <source>Poll status</source>
            <translation>Статус опроса</translation>
        </message>
        <message>
            <source>Read only</source>
            <translation>Только просмотр</translation>
        </message>
        <message>
            <source>Saving answer…</source>
            <translation>Сохранение ответа…</translation>
        </message>
    </context>
    <context>
        <name>NativeEncryptedChatInfo</name>
        <message>
            <source>Allow members to share keys</source>
            <translation>Разрешить участникам делиться ключами</translation>
        </message>
        <message numerus="yes">
            <source>%n participant</source>
            <translation>
                <numerusform>%n участник</numerusform>
                <numerusform>%n участника</numerusform>
                <numerusform>%n участников</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>%n participants</source>
            <translation>
                <numerusform>%n участник</numerusform>
                <numerusform>%n участника</numerusform>
                <numerusform>%n участников</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>%n online</source>
            <translation>
                <numerusform>%n в сети</numerusform>
                <numerusform>%n в сети</numerusform>
                <numerusform>%n в сети</numerusform>
            </translation>
        </message>
        <message>
            <source>Protected chat</source>
            <translation>Защищённый чат</translation>
        </message>
        <message>
            <source>Call</source>
            <translation>Позвонить</translation>
        </message>
        <message>
            <source>Settings</source>
            <translation>Настройки</translation>
        </message>
        <message>
            <source>Channel name</source>
            <translation>Название канала</translation>
        </message>
        <message>
            <source>Description (optional)</source>
            <translation>Описание</translation>
        </message>
        <message>
            <source>No description</source>
            <translation>Без описания</translation>
        </message>
        <message>
            <source>Channel details are currently read-only.</source>
            <translation>Параметры канала пока доступны только для просмотра.</translation>
        </message>
        <message>
            <source>Encryption keys</source>
            <translation>Ключи шифрования</translation>
        </message>
        <message>
            <source>Channel type</source>
            <translation>Тип канала</translation>
        </message>
        <message>
            <source>Closed, protected history</source>
            <translation>Закрытый, защищённая история переписки</translation>
        </message>
        <message>
            <source>Open</source>
            <translation>Открытый</translation>
        </message>
        <message>
            <source>Members</source>
            <translation>Участники</translation>
        </message>
        <message>
        <source>Owner</source>
        <translation>Владелец</translation>
    </message>
    <message>
        <source>Administrator</source>
        <translation>Администратор</translation>
    </message>
    <message>
        <source>Moderator</source>
        <translation>Модератор</translation>
    </message>
    <message>
        <source>Guest</source>
        <translation>Гость</translation>
    </message>
    <message>
        <source>Member</source>
        <translation>Участник</translation>
    </message>
    <message>
        <source>online</source>
        <translation>В сети</translation>
    </message>
    <message>
        <source>away</source>
        <translation>Отошёл</translation>
    </message>
    <message>
        <source>offline</source>
        <translation>Не в сети</translation>
    </message>
    <message>
        <source>do not disturb</source>
        <translation>Не беспокоить</translation>
    </message>
    <message>
        <source>Add members</source>
        <translation>Добавить участников</translation>
    </message>
    <message>
        <source>No members found</source>
        <translation>Участники не найдены</translation>
    </message>
    <message>
        <source>Remove from channel: %1</source>
        <translation>Удалить из канала: %1</translation>
    </message>
        <message>
            <source>Added to this device</source>
            <translation>Добавлен на это устройство</translation>
        </message>
        <message>
            <source>Added: %1</source>
            <translation>Добавлен: %1</translation>
        </message>
        <message>
            <source>Announced in chat</source>
            <translation>Объявлен в чате</translation>
        </message>
        <message>
            <source>Copy key ID</source>
            <translation>Копировать ID ключа</translation>
        </message>
        <message>
            <source>Created: %1</source>
            <translation>Создан: %1</translation>
        </message>
        <message>
            <source>Current</source>
            <translation>Актуальный</translation>
        </message>
        <message>
            <source>Current key ID</source>
            <translation>ID текущего ключа</translation>
        </message>
        <message>
            <source>Delete from this device</source>
            <translation>Удалить с этого устройства</translation>
        </message>
        <message>
            <source>Delete the saved key?</source>
            <translation>Удалить сохранённый ключ?</translation>
        </message>
        <message>
            <source>Messages encrypted with this key will be unavailable on this device until you obtain the key again. The key in the chat and on other devices will remain unchanged.</source>
            <translation>Сообщения, зашифрованные этим ключом, будут недоступны на этом устройстве до повторного получения ключа. Ключ в чате и на других устройствах сохранится.</translation>
        </message>
        <message>
            <source>Cancel</source>
            <translation>Отмена</translation>
        </message>
        <message>
            <source>Historical</source>
            <translation>Исторический</translation>
        </message>
        <message>
            <source>Key ID</source>
            <translation>ID ключа</translation>
        </message>
        <message>
            <source>Key unavailable</source>
            <translation>Ключ недоступен</translation>
        </message>
        <message>
            <source>Keys on this device (%1)</source>
            <translation>Ключи на этом устройстве (%1)</translation>
        </message>
        <message>
            <source>Loading…</source>
            <translation>Загрузка…</translation>
        </message>
        <message>
            <source>No saved keys for this chat on this device.</source>
            <translation>На этом устройстве нет сохранённых ключей этого чата.</translation>
        </message>
        <message>
            <source>Announcement in flight</source>
            <translation>Объявление отправлено</translation>
        </message>
        <message>
            <source>Announcement unresolved</source>
            <translation>Объявление не разрешено</translation>
        </message>
        <message>
            <source>Announcing…</source>
            <translation>Объявляется…</translation>
        </message>
        <message>
            <source>Check announcement</source>
            <translation>Проверить объявление</translation>
        </message>
        <message>
            <source>Discard rotation</source>
            <translation>Отменить ротацию</translation>
        </message>
        <message>
            <source>Discard the pending rotation?</source>
            <translation>Отменить ожидающую ротацию?</translation>
        </message>
        <message>
            <source>The rotation announcement may still land later. If it does, this device may never be able to read that rotation. This discard cannot be undone.</source>
            <translation>Объявление о ротации ещё может быть доставлено. Если это произойдёт, это устройство, возможно, никогда не сможет прочитать эту ротацию. Отмену нельзя отменить.</translation>
        </message>
        <message>
            <source>Not found in loaded history</source>
            <translation>Нет в загруженной истории</translation>
        </message>
        <message>
            <source>Reissue key</source>
            <translation>Перевыпустить ключ</translation>
        </message>
        <message>
            <source>Request current key</source>
            <translation>Запросить актуальный ключ</translation>
        </message>
        <message>
            <source>Request sent. The saved key remains available.</source>
            <translation>Запрос отправлен. Сохранённый ключ остаётся доступным.</translation>
        </message>
        <message>
            <source>Request sent. Waiting for the key.</source>
            <translation>Запрос отправлен. Ожидаем ключ.</translation>
        </message>
        <message>
            <source>Request this key again</source>
            <translation>Запросить этот ключ повторно</translation>
        </message>
        <message>
            <source>Retry</source>
            <translation>Повторить</translation>
        </message>
        <message>
            <source>Saved key</source>
            <translation>Сохранённый ключ</translation>
        </message>
        <message>
            <source>The current key is missing from this device.</source>
            <translation>Актуального ключа нет на этом устройстве.</translation>
        </message>
        <message>
            <source>This key is no longer saved on this device.</source>
            <translation>Этот ключ больше не сохранён на устройстве.</translation>
        </message>
        <message>
            <source>Unknown</source>
            <translation>Неизвестно</translation>
        </message>
</context>
    <context>
        <name>NativeDownloadCenter</name>
        <message>
            <source>Starting...</source>
            <translation>Запуск...</translation>
        </message>
        <message>
            <source>Ready</source>
            <translation>Готово</translation>
        </message>
        <message>
            <source>Failed</source>
            <translation>Ошибка</translation>
        </message>
        <message>
            <source>%1% - %2 / %3</source>
            <translation>%1% - %2 / %3</translation>
        </message>
        <message>
            <source>Downloading... %1</source>
            <translation>Загрузка... %1</translation>
        </message>
        <message>
            <source>Open downloads</source>
            <translation>Открыть загрузки</translation>
        </message>
        <message>
            <source>Downloads</source>
            <translation>Загрузки</translation>
        </message>
        <message>
            <source>Clear</source>
            <translation>Очистить</translation>
        </message>
        <message>
            <source>Retry download</source>
            <translation>Повторить загрузку</translation>
        </message>
        <message>
            <source>Open file</source>
            <translation>Открыть файл</translation>
        </message>
        <message>
            <source>Show in folder</source>
            <translation>Показать в папке</translation>
        </message>
        <message>
            <source>Cancel download</source>
            <translation>Отменить загрузку</translation>
        </message>
        <message>
            <source>Remove download</source>
            <translation>Удалить загрузку</translation>
        </message>
    </context>
    <context>
        <name>SidebarChatActions</name>
        <message>
            <source>Archive</source>
            <translation>Архивировать</translation>
        </message>
    </context>
    <context>
        <name>NativeForwardDialog</name>
        <message><source>Forward to channel</source><translation>Переслать в канал</translation></message>
        <message><source>Channel</source><translation>Канал</translation></message>
        <message><source>Select channel</source><translation>Выберите канал</translation></message>
        <message><source>Topic name</source><translation>Название темы</translation></message>
        <message><source>Select a topic to send messages to the channel</source><translation>Выберите тему для отправки сообщений в канал</translation></message>
        <message><source>Close</source><translation>Закрыть</translation></message>
        <message><source>Check</source><translation>Проверить</translation></message>
        <message><source>Cancel</source><translation>Отмена</translation></message>
        <message><source>Forward</source><translation>Переслать</translation></message>
        <message><source>Please wait…</source><translation>Подождите…</translation></message>
    </context>
    <context>
        <name>NativeLicensesPage</name>
        <message><source>Open Source Licenses</source><translation>Open Source лицензии</translation></message>
        <message><source>Workspace uses %1 open source packages</source><translation>Workspace использует %1 open source пакетов</translation></message>
        <message><source>Close</source><translation>Закрыть</translation></message>
        <message><source>Reset</source><translation>Сбросить</translation></message>
        <message><source>Search packages…</source><translation>Поиск пакетов…</translation></message>
        <message><source>%1 of %2 packages</source><translation>%1 из %2 пакетов</translation></message>
        <message><source>Package</source><translation>Пакет</translation></message>
        <message><source>Version</source><translation>Версия</translation></message>
        <message><source>License</source><translation>Лицензия</translation></message>
        <message><source>Author</source><translation>Автор</translation></message>
    </context>
    <context>
        <name>NativeAboutDrawer</name>
        <message><source>Open Source Licenses</source><translation>Open Source лицензии</translation></message>
    </context>
    <context>
        <name>NativeSupportReport</name>
        <message><source>Report a problem</source><translation>Сообщить о проблеме</translation></message>
        <message><source>Describe what you expected, what happened, when it happened and how to reproduce it. This description is kept separate from the diagnostic file.</source><translation>Опишите, что вы ожидали, что и когда произошло и как это воспроизвести. Описание сохраняется отдельно от файла диагностики.</translation></message>
        <message><source>Problem description</source><translation>Описание проблемы</translation></message>
        <message><source>Copy description</source><translation>Скопировать описание</translation></message>
        <message><source>Include session counts and response times</source><translation>Включить счётчики событий и время ответа за сессию</translation></message>
        <message><source>Preparing report…</source><translation>Подготовка отчёта…</translation></message>
        <message><source>Prepare report</source><translation>Подготовить отчёт</translation></message>
        <message><source>Diagnostic file preview</source><translation>Предпросмотр файла диагностики</translation></message>
        <message><source>Cancel</source><translation>Отмена</translation></message>
        <message><source>Save report</source><translation>Сохранить отчёт</translation></message>
        <message><source>JSON files (*.json)</source><translation>Файлы JSON (*.json)</translation></message>
        <message><source>Report saved</source><translation>Отчёт сохранён</translation></message>
        <message><source>Could not save the report</source><translation>Не удалось сохранить отчёт</translation></message>
    </context>
    <context>
        <name>NativeDiagnosticsPage</name>
        <message><source>Report a problem</source><translation>Сообщить о проблеме</translation></message>
        <message><source>Diagnostics</source><translation>Диагностика</translation></message>
        <message><source>Full runtime diagnostics with searchable session logs.</source><translation>Полная диагностика рантайма и сессионных логов с поиском.</translation></message>
        <message><source>Memory</source><translation>Память</translation></message>
        <message><source>Memory metrics are not available in this build.</source><translation>Метрики памяти недоступны в этой сборке.</translation></message>
        <message><source>Total app memory</source><translation>Память приложения</translation></message>
        <message><source>System free memory</source><translation>Свободная RAM системы</translation></message>
        <message><source>%1 total</source><translation>%1 всего</translation></message>
        <message><source>Memory usage</source><translation>Использование памяти</translation></message>
        <message><source>Processes</source><translation>Процессы</translation></message>
        <message><source>Type</source><translation>Тип</translation></message>
        <message><source>PID</source><translation>PID</translation></message>
        <message><source>Working set</source><translation>Рабочий набор</translation></message>
        <message><source>Peak</source><translation>Пик</translation></message>
        <message><source>CPU</source><translation>CPU</translation></message>
        <message><source>Refresh</source><translation>Обновить</translation></message>
        <message><source>Copy connection report</source><translation>Копировать отчёт о подключении</translation></message>
        <message><source>Copy snapshot</source><translation>Скопировать снимок</translation></message>
        <message><source>Snapshot copied</source><translation>Снимок скопирован</translation></message>
        <message><source>Copy failed</source><translation>Не удалось скопировать</translation></message>
        <message><source>Advanced snapshot</source><translation>Расширенный снимок</translation></message>
        <message><source>Healthy</source><translation>Норма</translation></message>
        <message><source>Degraded</source><translation>Проблемы</translation></message>
        <message><source>Offline</source><translation>Офлайн</translation></message>
        <message><source>Ping server</source><translation>Пинг сервера</translation></message>
        <message><source>Reachable (%1 ms)</source><translation>Доступен (%1 мс)</translation></message>
        <message><source>Unreachable</source><translation>Недоступен</translation></message>
        <message><source>Connection</source><translation>Подключение</translation></message>
        <message><source>Reconnect attempts: %1</source><translation>Попыток переподключения: %1</translation></message>
        <message><source>Realtime</source><translation>Realtime</translation></message>
        <message><source>Events received: %1</source><translation>Получено событий: %1</translation></message>
        <message><source>online</source><translation>в сети</translation></message>
        <message><source>Window visible</source><translation>Окно видимо</translation></message>
        <message><source>Window hidden</source><translation>Окно скрыто</translation></message>
        <message><source>Last %1 event %2s ago</source><translation>Последнее событие %1 %2 с назад</translation></message>
        <message><source>Session</source><translation>Сессия</translation></message>
        <message><source>Not signed in</source><translation>Не выполнен вход</translation></message>
        <message><source>%1 Workspace sessions</source><translation>%1 Workspace-сессий</translation></message>
        <message><source>Session expires in ~%1 min</source><translation>Сессия истекает через ~%1 мин</translation></message>
        <message><source>Runtime</source><translation>Runtime</translation></message>
        <message><source>Notifications</source><translation>Уведомления</translation></message>
        <message><source>OS/browser permission</source><translation>Разрешение ОС/браузера</translation></message>
        <message><source>Log counts</source><translation>Счётчики логов</translation></message>
        <message><source>Performance</source><translation>Производительность</translation></message>
        <message><source>No performance metrics recorded yet</source><translation>Метрики производительности пока не записаны</translation></message>
        <message><source>API median %1 ms · p95 %2 ms (%3 calls)</source><translation>API median %1 мс · p95 %2 мс (%3 вызовов)</translation></message>
        <message><source>Local cache</source><translation>Локальный кэш</translation></message>
        <message><source>Local cache unavailable</source><translation>Локальный кэш недоступен</translation></message>
        <message><source>%1 cached responses</source><translation>Кэшировано ответов: %1</translation></message>
        <message><source>Workspace cache owner present</source><translation>Workspace cache owner присутствует</translation></message>
        <message><source>No Workspace cache owner</source><translation>Workspace cache owner отсутствует</translation></message>
        <message><source>Stores</source><translation>Stores</translation></message>
        <message><source>%1 streams</source><translation>%1 каналов</translation></message>
        <message><source>%1 conversations</source><translation>%1 разговоров</translation></message>
        <message><source>%1 users</source><translation>%1 пользователей</translation></message>
        <!-- Issue #51: session log viewer modal (reference settings.logs* values). -->
        <message><source>View logs</source><translation>Просмотр логов</translation></message>
        <message><source>Logs</source><translation>Журнал</translation></message>
        <message><source>In-memory session logs (last 500 entries).</source><translation>Логи текущей сессии в памяти (последние 500 записей).</translation></message>
        <message><source>Clear logs</source><translation>Очистить журнал</translation></message>
        <message><source>Export logs</source><translation>Экспортировать логи</translation></message>
        <message><source>Close</source><translation>Закрыть</translation></message>
        <message><source>Search logs</source><translation>Поиск по логам</translation></message>
        <message><source>Search message, scope, runtime, or payload…</source><translation>Поиск по сообщению, scope, рантайму или payload…</translation></message>
        <message><source>Log source</source><translation>Источник логов</translation></message>
        <message><source>All sources</source><translation>Все источники</translation></message>
        <message><source>API</source><translation>API</translation></message>
        <message><source>Actions</source><translation>Действия</translation></message>
        <message><source>Console</source><translation>Консоль</translation></message>
        <message><source>Pipeline traces</source><translation>Pipeline-трейсы</translation></message>
        <message><source>App</source><translation>Приложение</translation></message>
        <message><source>Log level</source><translation>Уровень логов</translation></message>
        <message><source>All levels</source><translation>Все уровни</translation></message>
        <message><source>Debug</source><translation>Debug</translation></message>
        <message><source>Info</source><translation>Info</translation></message>
        <message><source>Warn</source><translation>Warn</translation></message>
        <message><source>Error</source><translation>Error</translation></message>
        <message><source>Log scope</source><translation>Scope логов</translation></message>
        <message><source>All scopes</source><translation>Все scope</translation></message>
        <message><source>All runtimes</source><translation>Все рантаймы</translation></message>
        <message><source>Reset filters</source><translation>Сбросить фильтры</translation></message>
        <message><source>%1 of %2 entries</source><translation>%1 из %2 записей</translation></message>
        <message><source>Recent errors</source><translation>Недавние ошибки</translation></message>
        <message><source>Session log entries</source><translation>Записи журнала сессии</translation></message>
        <message><source>No logs yet</source><translation>Записей пока нет</translation></message>
        <message><source>The logs could not be exported to the selected location. Pick another folder and try again.</source><translation>Не удалось экспортировать логи в выбранное расположение. Выберите другую папку и повторите попытку.</translation></message>
    </context>
    <context>
        <name>NativeFavoritesPage</name>
        <message><source>Favorites</source><translation>Избранное</translation></message>
        <message><source>Loading…</source><translation>Загрузка…</translation></message>
        <message><source>Could not open Favorites.</source><translation>Не удалось открыть Избранное.</translation></message>
        <message><source>Try again</source><translation>Попробовать снова</translation></message>
    </context>
<context>
    <name>NativeKeyRequests</name>
    <message>
        <source>Sending response</source>
        <translation>Отправляется ответ</translation>
    </message>
    <message>
        <source>Security key requests (%1)</source>
        <translation>Запросы ключа безопасности (%1)</translation>
    </message>
    <message>
        <source>Showing loaded history.</source>
        <translation>Показаны данные из загруженной истории.</translation>
    </message>
    <message>
        <source>No key requests</source>
        <translation>Нет запросов ключа</translation>
    </message>
    <message>
        <source>Incoming requests (%1)</source>
        <translation>Входящие запросы (%1)</translation>
    </message>
    <message>
        <source>Granted keys (%1)</source>
        <translation>Выданные ключи (%1)</translation>
    </message>
    <message>
        <source>Rejected requests (%1)</source>
        <translation>Отклонённые запросы (%1)</translation>
    </message>
    <message>
        <source>Grant all</source>
        <translation>Выдать всем</translation>
    </message>
    <message>
        <source>Grant access to %1</source>
        <translation>Выдать доступ: %1</translation>
    </message>
    <message>
        <source>Reject request from %1</source>
        <translation>Отклонить запрос: %1</translation>
    </message>
    <message>
        <source>Check</source>
        <translation>Проверить</translation>
    </message>
    <message>
        <source>Retry</source>
        <translation>Повторить</translation>
    </message>
    <message>
        <source>Could not send the key. Requests to retry: %1.</source>
        <translation>Не удалось отправить ключ. Запросов для повторной попытки: %1.</translation>
    </message>
</context>
    <context>
        <name>NativeCachedMessageSearch</name>
        <message><source>Search cached messages</source><translation>Поиск в сохранённых сообщениях</translation></message>
        <message><source>Searches history saved on this device for the selected account and project.</source><translation>Поиск по истории выбранного аккаунта и проекта, сохранённой на этом устройстве.</translation></message>
        <message><source>Search message text</source><translation>Поиск по тексту сообщения</translation></message>
        <message><source>Enter text to search.</source><translation>Введите текст для поиска.</translation></message>
        <message><source>The local message cache is unavailable.</source><translation>Локальный кэш сообщений недоступен.</translation></message>
        <message><source>Searching…</source><translation>Поиск…</translation></message>
        <message><source>No matches in cached messages.</source><translation>В сохранённых сообщениях совпадений нет.</translation></message>
        <message><source>Showing 50 matches. Refine your search for more specific results.</source><translation>Показано 50 совпадений. Уточните запрос.</translation></message>
        <message><source>Close</source><translation>Закрыть</translation></message>
    </context>
    <context>
        <name>ComposerHeight</name>
        <message><source>Resize message editor</source><translation>Изменить высоту поля сообщения</translation></message>
        <message><source>Expand message editor</source><translation>Развернуть поле сообщения</translation></message>
        <message><source>Collapse message editor</source><translation>Свернуть поле сообщения</translation></message>
    </context>
    <context>
        <name>NativeComposerTimePicker</name>
        <message><source>Cancel</source><translation>Отмена</translation></message>
        <message><source>Hours</source><translation>Часы</translation></message>
        <message><source>Insert</source><translation>Вставить</translation></message>
        <message><source>Insert date and time</source><translation>Вставить дату и время</translation></message>
        <message><source>Minutes</source><translation>Минуты</translation></message>
        <message><source>Next month</source><translation>Следующий месяц</translation></message>
        <message><source>Previous month</source><translation>Предыдущий месяц</translation></message>
        <message><source>Seconds</source><translation>Секунды</translation></message>
    </context>
    <context>
        <name>NativeComposerSnippets</name>
        <message><source>Cancel</source><translation>Отмена</translation></message>
        <message><source>Create new saved snippet</source><translation>Создать шаблон</translation></message>
        <message><source>Filter</source><translation>Фильтр</translation></message>
        <message><source>Filter snippets</source><translation>Фильтр шаблонов</translation></message>
        <message><source>No saved snippets</source><translation>Нет сохранённых шаблонов</translation></message>
        <message><source>Save</source><translation>Сохранить</translation></message>
        <message><source>Saved snippets</source><translation>Сохранённые шаблоны</translation></message>
        <message><source>Saved snippets are not connected yet.</source><translation>Сохранённые шаблоны пока недоступны.</translation></message>
        <message><source>Snippet content</source><translation>Содержимое шаблона</translation></message>
        <message><source>Snippet title</source><translation>Название шаблона</translation></message>
    </context>
    <context>
        <name>NativeComposerStickerPicker</name>
        <message><source>No sticker packs installed yet.</source><translation>Наборы стикеров пока не установлены.</translation></message>
        <message><source>No stickers found</source><translation>Стикеры не найдены</translation></message>
        <message><source>Search stickers</source><translation>Найти стикер</translation></message>
    </context>
    <context>
        <name>ComposerTools</name>
        <message><source>Emoji</source><translation>Эмодзи</translation></message>
        <message><source>Insert date and time</source><translation>Вставить дату и время</translation></message>
        <message><source>Saved snippets</source><translation>Сохранённые шаблоны</translation></message>
        <message><source>Stickers</source><translation>Стикеры</translation></message>
    </context>
<context>
    <name>NativeAccountPreferences</name>
        <message><source>Keyboard shortcuts</source><translation>Горячие клавиши</translation></message>
    <message><source>Theme settings</source><translation>Настройки темы</translation></message>
    <message><source>Stream and topic order</source><translation>Порядок стримов и топиков</translation></message>
    <message><source>Last message</source><translation>По последнему сообщению</translation></message>
    <message><source>Unread first</source><translation>Непрочитанные</translation></message>
    <message><source>By name</source><translation>По имени</translation></message>
    <message><source>Folder layout</source><translation>Отображение папок</translation></message>
    <message><source>Vertical rail</source><translation>Вертикальная панель</translation></message>
    <message><source>Horizontal tabs</source><translation>Горизонтальные вкладки</translation></message>
    <message><source>Chat list density</source><translation>Плотность списка чатов</translation></message>
    <message><source>Chat background</source><translation>Задний фон чата</translation></message>
    <message><source>Standard</source><translation>Стандартная</translation></message>
    <message><source>Compact</source><translation>Компактная</translation></message>
    <message><source>Chat click</source><translation>Клик по чату</translation></message>
    <message><source>What to open when clicking a chat</source><translation>Что открывать при клике на чат</translation></message>
    <message><source>General chat and topics</source><translation>Общий чат и топики</translation></message>
    <message><source>Latest topic</source><translation>Последний топик</translation></message>
    <message><source>Click the avatar to open the general chat</source><translation>Клик по аватару откроет общий чат</translation></message>
    <message><source>Notification sound</source><translation>Звуки уведомлений</translation></message>
    <message><source>Default</source><translation>По умолчанию</translation></message>
    <message><source>Subtle</source><translation>Тихий</translation></message>
    <message><source>Digital</source><translation>Цифровой</translation></message>
    <message><source>Glass</source><translation>Стекло</translation></message>
    <message><source>Pulse</source><translation>Пульс</translation></message>
    <message><source>Off</source><translation>Без звука</translation></message>
    <message><source>Language</source><translation>Язык</translation></message>
    <message><source>Auto sign-out</source><translation>Автовыход</translation></message>
    <message><source>After inactivity</source><translation>После неактивности</translation></message>
    <message><source>6 hours</source><translation>6 часов</translation></message>
    <message><source>12 hours</source><translation>12 часов</translation></message>
    <message><source>24 hours</source><translation>24 часа</translation></message>
    <message><source>3 days</source><translation>3 дня</translation></message>
    <message><source>7 days</source><translation>7 дней</translation></message>
    <message><source>Never</source><translation>Никогда</translation></message>
    <message><source>Light</source><translation>Светлая</translation></message>
    <message><source>Dark</source><translation>Тёмная</translation></message>
    <message><source>System</source><translation>Системная</translation></message>
    <message><source>Warm orange</source><translation>Тёплый оранжевый</translation></message>
    <message><source>Cold blue</source><translation>Холодный синий</translation></message>
    <message><source>Blue mist</source><translation>Синий туман</translation></message>
    <message><source>Emerald chat</source><translation>Изумрудный чат</translation></message>
</context>
<context>
    <name>NativeChatBackgroundPanel</name>
    <message><source>JPEG, PNG, WebP - up to 8 MB. Backgrounds stay on this device.</source><translation>JPEG, PNG, WebP — до 8 МБ. Фон хранится на этом устройстве.</translation></message>
    <message><source>Mountains</source><translation>Горы</translation></message>
    <message><source>Sea</source><translation>Море</translation></message>
    <message><source>Forest</source><translation>Лес</translation></message>
    <message><source>Processing image...</source><translation>Обработка…</translation></message>
    <message><source>Add your own image</source><translation>Добавить своё изображение</translation></message>
    <message><source>Add your own</source><translation>Добавить своё</translation></message>
    <message><source>Your image</source><translation>Своё изображение</translation></message>
    <message><source>Default background</source><translation>Стандартный фон</translation></message>
    <message><source>Choose a JPEG, PNG or WebP image.</source><translation>Выбери изображение JPEG, PNG или WebP.</translation></message>
    <message><source>The image is too large or empty. Choose a file up to 8 MB and 24 megapixels.</source><translation>Изображение слишком большое или пустое. Выбери файл до 8 МБ и до 24 мегапикселей.</translation></message>
    <message><source>Could not save the background. Your previous background was kept.</source><translation>Не удалось сохранить фон. Предыдущий фон оставлен.</translation></message>
    <message><source>Could not open the image. Try another file.</source><translation>Не удалось открыть изображение. Попробуй другой файл.</translation></message>
    <message><source>Choose a background image file</source><translation>Выбрать файл для фона</translation></message>
    <message><source>Apply background</source><translation>Применить фон</translation></message>
    <message><source>For all chats or only the chat currently open?</source><translation>Для всех чатов или только для чата, который сейчас открыт?</translation></message>
    <message><source>Selected background preview</source><translation>Предпросмотр выбранного фона</translation></message>
    <message><source>The default background will be restored.</source><translation>Будет восстановлен стандартный фон.</translation></message>
    <message><source>Open a chat to apply a background only to that chat.</source><translation>Открой нужный чат, чтобы применить фон только к нему.</translation></message>
    <message><source>Cancel</source><translation>Отмена</translation></message>
    <message><source>Only this chat</source><translation>Только этот чат</translation></message>
    <message><source>For all chats</source><translation>Все чаты</translation></message>
</context>
    <context>
        <name>NativeHotkeysDialog</name>
        <message><source>Activity</source><translation>Активность</translation></message>
        <message><source>Back</source><translation>Назад</translation></message>
        <message><source>Bold</source><translation>Жирный</translation></message>
        <message><source>Calendar</source><translation>Календарь</translation></message>
        <message><source>Calls</source><translation>Звонки</translation></message>
        <message><source>Cancel</source><translation>Отмена</translation></message>
        <message><source>Change theme</source><translation>Сменить тему</translation></message>
        <message><source>Chat information</source><translation>Информация о чате</translation></message>
        <message><source>Close</source><translation>Закрыть</translation></message>
        <message><source>Close or cancel</source><translation>Закрыть или отменить</translation></message>
        <message><source>Code</source><translation>Код</translation></message>
        <message><source>Composer</source><translation>Редактор</translation></message>
        <message><source>Could not save settings</source><translation>Не удалось сохранить настройки</translation></message>
        <message><source>Disable</source><translation>Отключить</translation></message>
        <message><source>Disabled</source><translation>Отключено</translation></message>
        <message><source>Edit last message</source><translation>Редактировать последнее сообщение</translation></message>
        <message><source>Enable keyboard shortcuts</source><translation>Включить горячие клавиши</translation></message>
        <message><source>Fixed shortcuts</source><translation>Фиксированные сочетания</translation></message>
        <message><source>Folder list</source><translation>Список папок</translation></message>
        <message><source>Forward</source><translation>Вперёд</translation></message>
        <message><source>Help</source><translation>Справка</translation></message>
        <message><source>Interface</source><translation>Интерфейс</translation></message>
        <message><source>Invalid shortcut</source><translation>Недопустимое сочетание</translation></message>
        <message><source>Italic</source><translation>Курсив</translation></message>
        <message><source>Keyboard shortcuts</source><translation>Горячие клавиши</translation></message>
        <message><source>Mail</source><translation>Почта</translation></message>
        <message><source>Messenger</source><translation>Мессенджер</translation></message>
        <message><source>Move between focus areas</source><translation>Переход между областями фокуса</translation></message>
        <message><source>Move focus (Shift+Tab moves backward)</source><translation>Переход по элементам (Shift+Tab — назад)</translation></message>
        <message><source>Navigation</source><translation>Навигация</translation></message>
        <message><source>No shortcuts found</source><translation>Сочетания не найдены</translation></message>
        <message><source>Press a key combination. Escape cancels.</source><translation>Нажмите сочетание клавиш. Escape отменяет запись.</translation></message>
        <message><source>Press a shortcut</source><translation>Нажмите сочетание</translation></message>
        <message><source>Quote message</source><translation>Цитировать сообщение</translation></message>
        <message><source>Reset</source><translation>Сбросить</translation></message>
        <message><source>Reset all</source><translation>Сбросить все</translation></message>
        <message><source>Save</source><translation>Сохранить</translation></message>
        <message><source>Search chats</source><translation>Поиск чатов</translation></message>
        <message><source>Search shortcuts</source><translation>Поиск сочетаний</translation></message>
        <message><source>Select text, focus a message with Tab, or point to it.</source><translation>Выделите текст, перейдите к сообщению клавишей Tab или наведите на него курсор.</translation></message>
        <message><source>Send or insert a line, depending on send settings</source><translation>Отправка или перенос строки согласно настройке отправки</translation></message>
        <message><source>Settings are stored on this device. Composer shortcuts apply while editing a message.</source><translation>Настройки сохраняются на этом устройстве. Сочетания редактора действуют при вводе сообщения.</translation></message>
        <message><source>Standard editing shortcuts, such as copy</source><translation>Стандартные операции редактирования, например копирование</translation></message>
        <message><source>Strikethrough</source><translation>Зачёркнутый</translation></message>
        <message><source>This shortcut is already in use</source><translation>Это сочетание уже используется</translation></message>
        <message><source>This shortcut is reserved</source><translation>Это сочетание зарезервировано</translation></message>
        <message><source>Toggle sidebar</source><translation>Боковая панель</translation></message>
        <message><source>When the composer is empty and not editing a message</source><translation>Когда поле пустое и редактирование сообщения не активно</translation></message>
    </context>
    <context>
        <name>NativeTopicContextMenu</name>
        <message>
            <source>Topic notifications</source>
            <translation>Уведомления темы</translation>
        </message>
        <message>
            <source>Mute</source>
            <translation>Заглушить</translation>
        </message>
        <message>
            <source>Default</source>
            <translation>По умолчанию</translation>
        </message>
        <message>
            <source>Unmute</source>
            <translation>Включить</translation>
        </message>
        <message>
            <source>Follow</source>
            <translation>Отслеживать</translation>
        </message>
        <message>
            <source>Mark as read</source>
            <translation>Отметить как прочитанное</translation>
        </message>
        <message>
            <source>Rename topic</source>
            <translation>Переименовать тему</translation>
        </message>
        <message>
            <source>Topic name</source>
            <translation>Название темы</translation>
        </message>
        <message>
            <source>Save</source>
            <translation>Сохранить</translation>
        </message>
        <message>
            <source>Cancel</source>
            <translation>Отмена</translation>
        </message>
        <message>
            <source>Mark topic as done</source>
            <translation>Отметить тему как выполненную</translation>
        </message>
        <message>
            <source>Mark topic as not done</source>
            <translation>Снять отметку выполнения темы</translation>
        </message>
    </context>

<context>
    <name>NativeExternalProviderDialog</name>
    <message><source>Credentials required</source><translation>Требуют обновления доступа</translation></message>
    <message><source>Loading history</source><translation>Загружают историю</translation></message>
    <message><source>Connecting</source><translation>Подключаются</translation></message>
    <message><source>Degraded</source><translation>Работают с ошибками</translation></message>
    <message><source>Disconnected</source><translation>Отключены</translation></message>
    <message><source>Operational</source><translation>Работают</translation></message>
    <message><source>Suspended</source><translation>Приостановлены</translation></message>
    <message><source>Active</source><translation>Активны</translation></message>
    <message><source>Enrolling</source><translation>Регистрируются</translation></message>
    <message><source>Incompatible</source><translation>Несовместимы</translation></message>
    <message><source>Revoked</source><translation>Отозваны</translation></message>
    <message><source>Available to connect</source><translation>Доступны для подключения</translation></message>
    <message><source>Connected</source><translation>Подключены</translation></message>
    <message><source>Discarded</source><translation>Отброшены</translation></message>
    <message><source>Failed</source><translation>Завершились с ошибкой</translation></message>
    <message><source>Manual reconciliation required</source><translation>Требуют ручной проверки</translation></message>
    <message><source>Queued</source><translation>В очереди</translation></message>
    <message><source>Running</source><translation>Выполняются</translation></message>
    <message><source>Succeeded</source><translation>Завершены</translation></message>
    <message><source>Queued and running operations</source><translation>Операции в очереди и работе</translation></message>
    <message><source>Selected chats</source><translation>Выбрано чатов</translation></message>
    <message><source>Synchronized messages</source><translation>Синхронизировано сообщений</translation></message>
    <message><source>Synchronized users</source><translation>Синхронизировано пользователей</translation></message>
    <message><source>No data</source><translation>Нет данных</translation></message>
    <message><source>External integration settings</source><translation>Настройки внешних интеграций</translation></message>
    <message><source>Close</source><translation>Закрыть</translation></message>
    <message><source>Loading integration settings…</source><translation>Загружаем настройки интеграции…</translation></message>
    <message><source>Could not load the integration settings.</source><translation>Не удалось загрузить настройки интеграции.</translation></message>
    <message><source>Refresh</source><translation>Обновить</translation></message>
    <message><source>Another administrator changed these settings. Your draft is preserved in the form.</source><translation>Настройки уже изменил другой администратор. Ваши правки сохранены в форме.</translation></message>
    <message><source>Load current settings</source><translation>Загрузить актуальные</translation></message>
    <message><source>Keep my draft</source><translation>Оставить мои правки</translation></message>
    <message><source>Overview</source><translation>Общее состояние</translation></message>
    <message><source>Provider</source><translation>Провайдер</translation></message>
    <message><source>Technical status</source><translation>Техническое состояние</translation></message>
    <message><source>Unavailable</source><translation>Недоступен</translation></message>
    <message><source>Availability</source><translation>Доступность</translation></message>
    <message><source>Integration allowed</source><translation>Интеграция разрешена</translation></message>
    <message><source>Integration disabled</source><translation>Интеграция запрещена</translation></message>
    <message><source>Emergency suspension</source><translation>Аварийная остановка</translation></message>
    <message><source>Integration suspended</source><translation>Интеграция приостановлена</translation></message>
    <message><source>Not active</source><translation>Не действует</translation></message>
    <message><source>Updated: %1</source><translation>Обновлено: %1</translation></message>
    <message><source>Availability and limits</source><translation>Доступность и лимиты</translation></message>
    <message><source>Limits apply to new accounts, selected chats, and transferred files.</source><translation>Ограничения применяются к новым аккаунтам, чатам и передаваемым файлам.</translation></message>
    <message><source>Allow the Zulip integration</source><translation>Разрешить интеграцию Zulip</translation></message>
    <message><source>Allows users in this organization to connect Zulip accounts.</source><translation>Разрешает пользователям подключать аккаунты Zulip в этой организации.</translation></message>
    <message><source>Accounts per user</source><translation>Аккаунтов на пользователя</translation></message>
    <message><source>Chats per external account</source><translation>Чатов на внешний аккаунт</translation></message>
    <message><source>Maximum file size</source><translation>Максимальный размер файла</translation></message>
    <message><source>MiB</source><translation>МиБ</translation></message>
    <message><source>Enter a value from 0 to %1.</source><translation>Укажите значение от 0 до %1.</translation></message>
    <message><source>Lowering these limits does not remove existing accounts or selected chats.</source><translation>Уменьшение лимитов не удаляет уже подключённые аккаунты и выбранные чаты.</translation></message>
    <message><source>Availability and limits cannot be changed while a custom certificate bundle is installed. The backend cannot safely preserve it without receiving the certificates again.</source><translation>Пока установлен собственный набор сертификатов, изменение доступности и лимитов недоступно. Серверная часть не поддерживает безопасное сохранение этих полей без повторной передачи сертификатов.</translation></message>
    <message><source>Could not save the integration settings.</source><translation>Не удалось сохранить настройки интеграции.</translation></message>
    <message><source>Custom certificate authority</source><translation>Собственный центр сертификации</translation></message>
    <message><source>No custom certificate bundle is installed.</source><translation>Собственный набор сертификатов не установлен.</translation></message>
    <message><source>Certificates</source><translation>Сертификатов</translation></message>
    <message><source>Generation</source><translation>Поколение</translation></message>
    <message><source>SHA-256 fingerprint</source><translation>Отпечаток SHA-256</translation></message>
    <message><source>Loading technical status…</source><translation>Загружаем техническое состояние…</translation></message>
    <message><source>Could not load the technical status.</source><translation>Не удалось загрузить техническое состояние.</translation></message>
    <message><source>Could not refresh the technical status.</source><translation>Не удалось обновить техническое состояние.</translation></message>
    <message><source>Accounts</source><translation>Аккаунты</translation></message>
    <message><source>Chats</source><translation>Чаты</translation></message>
    <message><source>Bridge instances</source><translation>Экземпляры моста</translation></message>
    <message><source>Operations</source><translation>Операции</translation></message>
    <message><source>Metrics</source><translation>Метрики</translation></message>
    <message><source>Emergency controls</source><translation>Аварийное управление</translation></message>
    <message><source>These actions affect the Zulip integration for every user in the organization.</source><translation>Эти действия влияют на интеграцию Zulip для всех пользователей организации.</translation></message>
    <message><source>Could not complete the action. The form settings were not changed.</source><translation>Не удалось выполнить действие. Настройки формы не изменены.</translation></message>
    <message><source>Suspending…</source><translation>Приостанавливаем…</translation></message>
    <message><source>Suspend integration</source><translation>Приостановить интеграцию</translation></message>
    <message><source>Resuming…</source><translation>Возобновляем…</translation></message>
    <message><source>Resume integration</source><translation>Возобновить интеграцию</translation></message>
    <message><source>Suspend the Zulip integration?</source><translation>Приостановить интеграцию Zulip?</translation></message>
    <message><source>Zulip connections will be suspended for every user in the organization.</source><translation>Подключение Zulip будет приостановлено для всех пользователей организации.</translation></message>
    <message><source>Suspend for everyone</source><translation>Приостановить для всех</translation></message>
    <message><source>Cancel</source><translation>Отмена</translation></message>
    <message><source>Saving…</source><translation>Сохраняем…</translation></message>
    <message><source>Save</source><translation>Сохранить</translation></message>
    </context>
<context>
    <name>NativeTopicSummarySettingsDialog</name>
    <message>
        <source>AI summary settings</source>
        <translation>Настройки AI-выжимок</translation>
    </message>
    <message>
        <source>Close</source>
        <translation>Закрыть</translation>
    </message>
    <message>
        <source>This topic</source>
        <translation>Этот топик</translation>
    </message>
    <message>
        <source>Common settings</source>
        <translation>Общие настройки</translation>
    </message>
    <message>
        <source>LLM endpoints</source>
        <translation>LLM endpoint-ы</translation>
    </message>
    <message>
        <source>You do not have permission to change these settings.</source>
        <translation>У вас нет права изменять эти настройки.</translation>
    </message>
    <message>
        <source>Automatic summary updates</source>
        <translation>Автоматическая генерация выжимки</translation>
    </message>
    <message>
        <source>System prompt</source>
        <translation>Системный промпт</translation>
    </message>
    <message>
        <source>Leave empty to use the default prompt.</source>
        <translation>Можно оставить пустым, чтобы использовать стандартный промпт.</translation>
    </message>
    <message>
        <source>Use default prompt</source>
        <translation>Использовать стандартный промпт</translation>
    </message>
    <message>
        <source>The prompt must not be empty.</source>
        <translation>Введите свой промпт или выберите стандартный.</translation>
    </message>
    <message>
        <source>The prompt is too long (maximum %1 characters).</source>
        <translation>Промпт должен быть не длиннее %1 символов.</translation>
    </message>
    <message>
        <source>Reasoning effort</source>
        <translation>Уровень рассуждения</translation>
    </message>
    <message>
        <source>Provider default</source>
        <translation>По умолчанию провайдера</translation>
    </message>
    <message>
        <source>Could not update the topic. Please try again.</source>
        <translation>Не удалось обновить топик. Попробуйте ещё раз.</translation>
    </message>
    <message>
        <source>Generation is active</source>
        <translation>Генерация активна</translation>
    </message>
    <message>
        <source>Generation is paused</source>
        <translation>Генерация приостановлена</translation>
    </message>
    <message>
        <source>Global summary generation</source>
        <translation>Включено для всей системы</translation>
    </message>
    <message>
        <source>Enable summaries in this workspace</source>
        <translation>Включить выжимки для этого проекта</translation>
    </message>
    <message>
        <source>Add endpoint</source>
        <translation>Добавить endpoint</translation>
    </message>
    <message>
        <source>Edit</source>
        <translation>Изменить</translation>
    </message>
    <message>
        <source>Delete</source>
        <translation>Удалить</translation>
    </message>
    <message>
        <source>Delete this endpoint? Existing summaries are not affected.</source>
        <translation>Удалить этот endpoint? Существующие выжимки не будут затронуты.</translation>
    </message>
    <message>
        <source>Cancel</source>
        <translation>Отмена</translation>
    </message>
    <message>
        <source>Name *</source>
        <translation>Название *</translation>
    </message>
    <message>
        <source>Model *</source>
        <translation>Модель *</translation>
    </message>
    <message>
        <source>Base URL (https://...) *</source>
        <translation>Базовый URL (https://...) *</translation>
    </message>
    <message>
        <source>API key *</source>
        <translation>API-ключ *</translation>
    </message>
    <message>
        <source>Priority (0-1000000)</source>
        <translation>Приоритет (0-1000000)</translation>
    </message>
    <message>
        <source>Temperature (0-2)</source>
        <translation>Температура (0-2)</translation>
    </message>
    <message>
        <source>Max output tokens (1-32768)</source>
        <translation>Лимит выходных токенов (1-32768)</translation>
    </message>
    <message>
        <source>Top-p (0-1)</source>
        <translation>Top P (0-1)</translation>
    </message>
    <message>
        <source>Presence penalty (-2..2)</source>
        <translation>Штраф за присутствие (-2..2)</translation>
    </message>
    <message>
        <source>Frequency penalty (-2..2)</source>
        <translation>Штраф за частоту (-2..2)</translation>
    </message>
    <message>
        <source>Priority</source>
        <translation>Приоритет</translation>
    </message>
    <message>
        <source>Temperature</source>
        <translation>Температура</translation>
    </message>
    <message>
        <source>Output token limit</source>
        <translation>Лимит выходных токенов</translation>
    </message>
    <message>
        <source>Top P</source>
        <translation>Top P</translation>
    </message>
    <message>
        <source>Presence penalty</source>
        <translation>Штраф за присутствие</translation>
    </message>
    <message>
        <source>Frequency penalty</source>
        <translation>Штраф за частоту</translation>
    </message>
    <message>
        <source>Range: 0-1000000</source>
        <translation>Диапазон: 0-1000000</translation>
    </message>
    <message>
        <source>Range: 0-2</source>
        <translation>Диапазон: 0-2</translation>
    </message>
    <message>
        <source>Range: 1-32768</source>
        <translation>Диапазон: 1-32768</translation>
    </message>
    <message>
        <source>Range: 0-1</source>
        <translation>Диапазон: 0-1</translation>
    </message>
    <message>
        <source>Range: -2..2</source>
        <translation>Диапазон: -2..2</translation>
    </message>
    <message>
        <source>Enabled</source>
        <translation>Включен</translation>
    </message>
    <message>
        <source>Supports vision</source>
        <translation>Поддерживает изображения</translation>
    </message>
    <message>
        <source>Supports reasoning</source>
        <translation>Поддерживает управление reasoning</translation>
    </message>
    <message>
        <source>Unsaved changes</source>
        <translation>Есть несохранённые изменения</translation>
    </message>
    <message>
        <source>Saved</source>
        <translation>Сохранено</translation>
    </message>
    <message>
        <source>Save</source>
        <translation>Сохранить</translation>
    </message>
    <message>
        <source>Network error. Your changes were kept, try again.</source>
        <translation>Ошибка сети. Изменения сохранены для повторной попытки.</translation>
    </message>
    <message>
        <source>The settings operation was interrupted by a session refresh. Try again.</source>
        <translation>Изменение настроек прервано обновлением сессии. Повторите попытку.</translation>
    </message>
    <message>
        <source>The server rejected the values.</source>
        <translation>Проверьте введенные значения.</translation>
    </message>
    <message>
        <source>The server returned an unexpected response.</source>
        <translation>Сервер вернул некорректный ответ.</translation>
    </message>
    <message>
        <source>The server failed. Please try again.</source>
        <translation>Сервер не смог сохранить настройки.</translation>
    </message>
    <message>
        <source>Something went wrong. Please try again.</source>
        <translation>Не удалось выполнить операцию.</translation>
    </message>
    <message><source>This field is required.</source><translation>Обязательное поле.</translation></message>
    <message><source>The value is too long.</source><translation>Значение слишком длинное.</translation></message>
    <message><source>Check the entered values.</source><translation>Проверьте введенные значения.</translation></message>
    <message><source>Enter a whole number.</source><translation>Введите целое число.</translation></message>
    <message><source>Enter a valid value.</source><translation>Введите корректное значение.</translation></message>
    <message><source>The value is outside the allowed range.</source><translation>Значение выходит за допустимый диапазон.</translation></message>
    <message><source>New API key</source><translation>Новый API-ключ</translation></message>
    <message><source>Leave blank to keep the current write-only credential.</source><translation>Оставьте поле пустым, чтобы сохранить текущий секретный ключ.</translation></message>
    <message><source>Required. The key is encrypted and never shown again.</source><translation>Обязательное поле. Ключ будет зашифрован и больше не отобразится.</translation></message>
</context>
    <context>
        <name>NativeAccountMenu</name>
        <message>
            <source>App version</source>
            <translation>Версия приложения</translation>
        </message>
        <message>
            <source>Appearance</source>
            <translation>Оформление</translation>
        </message>
        <message>
            <source>Clear cache</source>
            <translation>Очистить кэш</translation>
        </message>
        <message>
            <source>Current server</source>
            <translation>Текущий сервер</translation>
        </message>
        <message>
            <source>Description</source>
            <translation>Описание</translation>
        </message>
        <message>
            <source>Diagnostics</source>
            <translation>Диагностика</translation>
        </message>
        <message>
            <source>Disk cache (GB)</source>
            <translation>Кэш на диске (ГБ)</translation>
        </message>
        <message>
            <source>Disk cache is unavailable.</source>
            <translation>Дисковый кэш недоступен.</translation>
        </message>
        <message>
            <source>Disk cache limit in GB</source>
            <translation>Лимит кэша на диске в ГБ</translation>
        </message>
        <message>
            <source>English</source>
            <translation>English</translation>
        </message>
        <message>
            <source>Language</source>
            <translation>Язык</translation>
        </message>
        <message>
            <source>Message cache, local settings, then reload</source>
            <translation>Кэш сообщений, локальные настройки, перезагрузка</translation>
        </message>
        <message>
            <source>Notification sound, Language, Auto sign-out</source>
            <translation>Звуки уведомлений, Язык, Автовыход</translation>
        </message>
        <message>
            <source>Personal info</source>
            <translation>Личная информация</translation>
        </message>
        <message>
            <source>Refresh</source>
            <translation>Обновить</translation>
        </message>
        <message>
            <source>Requested: %1 GB · Applied: %2 GB</source>
            <translation>Задано: %1 ГБ · Применено: %2 ГБ</translation>
        </message>
        <message>
            <source>Requested: %1 GB</source>
            <translation>Задано: %1 ГБ</translation>
        </message>
        <message>
            <source>Settings</source>
            <translation>Настройки</translation>
        </message>
        <message>
            <source>Sign out</source>
            <translation>Выйти</translation>
        </message>
        <message>
            <source>Status</source>
            <translation>Статус</translation>
        </message>
        <message>
            <source>Theme settings</source>
            <translation>Настройки темы</translation>
        </message>
        <message>
            <source>Updating disk cache…</source>
            <translation>Обновление дискового кэша…</translation>
        </message>
        <message>
            <source>Used: %1</source>
            <translation>Занято: %1</translation>
        </message>
        <message>
            <source>Русский</source>
            <translation>Русский</translation>
        </message>
    </context>
    <context>
        <name>NativeCalendarPage</name>
        <message>
            <source>Sample calendar</source>
            <translation>Демонстрационный календарь</translation>
        </message>
        <message>
            <source>Create meeting</source>
            <translation>Создать встречу</translation>
        </message>
        <message>
            <source>Day</source>
            <translation>День</translation>
        </message>
        <message>
            <source>Week</source>
            <translation>Неделя</translation>
        </message>
        <message>
            <source>Month</source>
            <translation>Месяц</translation>
        </message>
        <message>
            <source>Today</source>
            <translation>Сегодня</translation>
        </message>
        <message>
            <source>Loading...</source>
            <translation>Загрузка...</translation>
        </message>
        <message>
            <source>Could not load the calendar.</source>
            <translation>Не удалось загрузить календарь.</translation>
        </message>
        <message>
            <source>Retry</source>
            <translation>Повторить</translation>
        </message>
        <message>
            <source>Work</source>
            <translation>Работа</translation>
        </message>
        <message>
            <source>Personal</source>
            <translation>Личное</translation>
        </message>
        <message>
            <source>No events on this day.</source>
            <translation>На этот день событий нет.</translation>
        </message>
    </context>
    <context>
        <name>NativeCalendarEventDetail</name>
        <message>
            <source>(untitled)</source>
            <translation>(без названия)</translation>
        </message>
        <message>
            <source>Location</source>
            <translation>Место</translation>
        </message>
        <message>
            <source>No location specified</source>
            <translation>Место не указано</translation>
        </message>
        <message>
            <source>Participants</source>
            <translation>Участники</translation>
        </message>
        <message>
            <source>+%1 more</source>
            <translation>и ещё %1</translation>
        </message>
    </context>
    <context>
        <name>NativeCalendarMeetingDialog</name>
        <message>
            <source>New meeting</source>
            <translation>Новая встреча</translation>
        </message>
        <message>
            <source>Enter a meeting title.</source>
            <translation>Введите название встречи.</translation>
        </message>
        <message>
            <source>Enter valid dates and times. The end must be after the start.</source>
            <translation>Проверьте даты и время. Встреча должна закончиться позже, чем начнётся.</translation>
        </message>
        <message>
            <source>The sample calendar is unavailable.</source>
            <translation>Демонстрационный календарь недоступен.</translation>
        </message>
        <message>
            <source>Sample meetings stay in this session. Participants will not receive invitations.</source>
            <translation>Встречи сохраняются на время этого сеанса. Участникам не придут приглашения.</translation>
        </message>
        <message>
            <source>Title</source>
            <translation>Название</translation>
        </message>
        <message>
            <source>Participants</source>
            <translation>Участники</translation>
        </message>
        <message>
            <source>One name per line</source>
            <translation>По одному имени на строку</translation>
        </message>
        <message>
            <source>Calendar</source>
            <translation>Календарь</translation>
        </message>
        <message>
            <source>Work</source>
            <translation>Рабочий</translation>
        </message>
        <message>
            <source>Personal</source>
            <translation>Личный</translation>
        </message>
        <message>
            <source>Start date</source>
            <translation>Дата начала</translation>
        </message>
        <message>
            <source>Start time</source>
            <translation>Время начала</translation>
        </message>
        <message>
            <source>End date</source>
            <translation>Дата окончания</translation>
        </message>
        <message>
            <source>End time</source>
            <translation>Время окончания</translation>
        </message>
        <message>
            <source>Location</source>
            <translation>Место</translation>
        </message>
        <message>
            <source>Create meeting</source>
            <translation>Создать встречу</translation>
        </message>
        <message>
            <source>Cancel</source>
            <translation>Отмена</translation>
        </message>
    </context>
</TS>
